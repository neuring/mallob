
#pragma once

#include <future>

#include "util/params.hpp"
#include "util/hashing.hpp"
#include "../sharing/buffer/adaptive_clause_database.hpp"
#include "data/job_transfer.hpp"
#include "app/job.hpp"
#include "base_sat_job.hpp"
#include "clause_history.hpp"
//#include "distributed_clause_filter.hpp"
#include "comm/job_tree_all_reduction.hpp"
#include "../proof/proof_assembler.hpp"
#include "../proof/merging/distributed_proof_merger.hpp"
#include "../proof/merging/proof_merge_connector.hpp"
#include "util/small_merger.hpp"

class AnytimeSatClauseCommunicator {

public:
    enum Stage {
        PREPARING_CLAUSES, 
        MERGING, 
        WAITING_FOR_CLAUSE_BCAST, 
        PREPARING_FILTER, 
        WAITING_FOR_FILTER_BCAST
    };

private:
    const Parameters& _params;
    BaseSatJob* _job = NULL;
    bool _suspended = false;

    const int _clause_buf_base_size;
    const float _clause_buf_discount_factor;
    const bool _use_cls_history;

    AdaptiveClauseDatabase _cdb;
    ClauseHistory _cls_history;
    //DistributedClauseFilter _filter;
    float _compensation_factor = 1.0f;
    float _compensation_decay = 0.6;

    struct Session {

        const Parameters& _params;
        BaseSatJob* _job;
        AdaptiveClauseDatabase& _cdb;
        int _epoch;

        std::vector<int> _excess_clauses_from_merge;
        std::vector<int> _broadcast_clause_buffer;
        int _num_broadcast_clauses;
        int _num_admitted_clauses;

        JobTreeAllReduction _allreduce_clauses;
        JobTreeAllReduction _allreduce_filter;
        bool _filtering = false;
        bool _all_stages_done = false;

        Session(const Parameters& params, BaseSatJob* job, AdaptiveClauseDatabase& cdb, int epoch) : 
            _params(params), _job(job), _cdb(cdb), _epoch(epoch),
            _allreduce_clauses(
                job->getJobTree(),
                // Base message 
                JobMessage(_job->getId(), _job->getRevision(), epoch, MSG_ALLREDUCE_CLAUSES),
                // Neutral element
                {1, -1}, // two integers: number of aggregated job tree nodes, winning solver ID
                // Aggregator for local + incoming elements
                [&](std::list<std::vector<int>>& elems) {
                    int numAggregated = 0;
                    int successfulSolverId = -1;
                    for (auto& elem : elems) {
                        if (elem.back() != -1 && (successfulSolverId == -1 || successfulSolverId > elem.back())) {
                            successfulSolverId = elem.back();
                        }
                        elem.pop_back();
                        numAggregated += elem.back();
                        elem.pop_back();
                    }
                    auto merger = _cdb.getBufferMerger(_job->getBufferLimit(numAggregated, MyMpi::ALL));
                    for (auto& elem : elems) {
                        merger.add(_cdb.getBufferReader(elem.data(), elem.size()));
                    }
                    std::vector<int> merged = merger.merge(&_excess_clauses_from_merge);
                    LOG(V4_VVER, "%s : merged %i contribs ~> len=%i\n", 
                        _job->toStr(), numAggregated, merged.size());
                    merged.push_back(numAggregated);
                    merged.push_back(successfulSolverId);
                    return merged;
                }
            ),
            _allreduce_filter(
                job->getJobTree(), 
                // Base message
                JobMessage(_job->getId(), _job->getRevision(), epoch, MSG_ALLREDUCE_FILTER),
                // Neutral element
                std::vector<int>(ClauseMetadata::numBytes(), 0),
                // Aggregator for local + incoming elements
                [&](std::list<std::vector<int>>& elems) {
                    std::vector<int> filter = std::move(elems.front());
                    elems.pop_front();

                    unsigned long maxMinEpochId;
                    if (ClauseMetadata::enabled()) {
                        assert(filter.size() >= 2);
                        maxMinEpochId = ClauseMetadata::readUnsignedLong(filter.data());
                    }

                    for (auto& elem : elems) {
                        if (filter.size() < elem.size()) 
                            filter.resize(elem.size());

                        if (ClauseMetadata::enabled()) {
                            assert(elem.size() >= 2);
                            unsigned long minEpochId = ClauseMetadata::readUnsignedLong(elem.data());
                            maxMinEpochId = std::max(maxMinEpochId, minEpochId);
                        }
                        
                        for (size_t i = ClauseMetadata::numBytes(); i < elem.size(); i++) {
                            filter[i] |= elem[i]; // bitwise OR
                        }
                    }

                    if (ClauseMetadata::enabled()) {
                        ClauseMetadata::writeUnsignedLong(maxMinEpochId, filter.data());
                    }

                    return filter;
                }
            ) { }
        ~Session() {
            _allreduce_clauses.destroy();
            _allreduce_filter.destroy();
        }

        void setFiltering() {_filtering = true;}
        bool isFiltering() const {return _filtering;}
        std::vector<int> applyGlobalFilter(const std::vector<int>& filter, std::vector<int>& clauses);
        void setAllStagesDone() {_all_stages_done = true;}

        bool isValid() const {
            return _allreduce_clauses.isValid() || _allreduce_filter.isValid();
        }
        bool allStagesDone() const {return _all_stages_done;}
        bool isDestructible() {
            return _allreduce_clauses.isDestructible() && _allreduce_filter.isDestructible();
        }
    };

    std::list<Session> _sessions;

    int _current_epoch = 0;
    float _time_of_last_epoch_initiation = 0;
    float _time_of_last_epoch_conclusion = 0.000001f;

    float _solving_time = 0;
    float _reconstruction_time = 0;

    bool _sent_ready_msg;
    int _num_ready_msgs_from_children = 0;

    JobMessage _msg_unsat_found;
    std::list<JobMessage> _deferred_sharing_initiation_msgs;

    bool _initiated_proof_assembly = false;
    std::optional<ProofAssembler> _proof_assembler;
    std::optional<JobTreeAllReduction> _proof_all_reduction;
    bool _done_assembling_proof = false;
    std::vector<int> _proof_all_reduction_result;

    std::unique_ptr<DistributedProofMerger> _file_merger;
    std::vector<std::unique_ptr<MergeSourceInterface<SerializedLratLine>>> _local_merge_inputs;
    std::unique_ptr<SmallMerger<SerializedLratLine>> _local_merger;
    std::vector<ProofMergeConnector*> _merge_connectors;

public:
    AnytimeSatClauseCommunicator(const Parameters& params, BaseSatJob* job) : _params(params), _job(job), 
        _clause_buf_base_size(_params.clauseBufferBaseSize()), 
        _clause_buf_discount_factor(_params.clauseBufferDiscountFactor()),
        _use_cls_history(params.collectClauseHistory()),
        _cdb([&]() {
            AdaptiveClauseDatabase::Setup setup;
            setup.maxClauseLength = _params.strictClauseLengthLimit();
            setup.maxLbdPartitionedSize = _params.maxLbdPartitioningSize();
            setup.slotsForSumOfLengthAndLbd = _params.groupClausesByLengthLbdSum();
            setup.numLiterals = 0;
            return setup;
        }()),
        _cls_history(_params, _job->getBufferLimit(_job->getJobTree().getCommSize(), MyMpi::ALL), *job, _cdb), _sent_ready_msg(!ClauseMetadata::enabled() && !_params.deterministicSolving()) {

        _time_of_last_epoch_initiation = Timer::elapsedSeconds();
        _time_of_last_epoch_conclusion = Timer::elapsedSeconds();
    }

    ~AnytimeSatClauseCommunicator() {
        _sessions.clear();
    }

    void communicate();
    void handle(int source, int mpiTag, JobMessage& msg);
    void feedHistoryIntoSolver();
    bool isDestructible() {
        for (auto& session : _sessions) if (!session.isDestructible()) return false;
        return true;
    }
    int getCurrentEpoch() const {return _current_epoch;}

    bool isDoneAssemblingProof() const {return _done_assembling_proof;}

private:

    std::vector<ProofMergeConnector*> setUpProofMerger(int threadsPerWorker);

    inline Session& currentSession() {return _sessions.back();}
    void addToClauseHistory(std::vector<int>& clauses, int epoch);
    void createNewProofAllReduction();

    void initiateClauseSharing(JobMessage& msg);
    void tryActivateDeferredSharingInitiation();
};
