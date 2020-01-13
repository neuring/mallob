
#ifndef DOMPASCH_CUCKOO_REBALANCER_WORKER
#define DOMPASCH_CUCKOO_REBALANCER_WORKER

#include <set>
#include <chrono>
#include <string>
#include <thread>
#include <memory>

#include "HordeLib.h"

#include "util/mympi.h"
#include "util/params.h"
#include "data/job.h"
#include "data/job_description.h"
#include "data/job_transfer.h"
#include "data/epoch_counter.h"
#include "data/statistics.h"
#include "balancing/balancer.h"

class Worker {

private:
    MPI_Comm comm;
    int worldRank;
    std::set<int> clientNodes;
    Parameters& params;

    float loadFactor;

    std::map<int, Job*> jobs;
    std::map<int, JobRequest> jobCommitments;
    std::map<int, float> jobArrivals;
    std::map<int, float> jobCpuTimeUsed;
    Job* currentJob;
    int load;
    float lastLoadChange;
    std::map<int, int> jobVolumes;

    std::unique_ptr<Balancer> balancer;
    EpochCounter epochCounter;
    Statistics stats;

    std::map<int, std::thread> initializerThreads;

    std::vector<int> bounceAlternatives;

    std::thread mpiMonitorThread;
    bool exiting;

public:
    Worker(MPI_Comm comm, Parameters& params, const std::set<int>& clientNodes) :
        comm(comm), worldRank(MyMpi::rank(MPI_COMM_WORLD)), clientNodes(clientNodes), params(params), epochCounter(), stats(epochCounter)
        {
            loadFactor = params.getFloatParam("l");
            assert(0 < loadFactor && loadFactor < 1.0);
            load = 0;
            lastLoadChange = Timer::elapsedSeconds();
            currentJob = NULL;
            exiting = false;
        }

    ~Worker();
    void init();
    void mainProgram();
    void dumpStats() {//stats.dump();
    };

private:
    friend void mpiMonitor(Worker* worker);

    bool checkTerminate();

    void handleIntroduceJob(MessageHandlePtr& handle);
    void handleQueryVolume(MessageHandlePtr& handle);
    void handleFindNode(MessageHandlePtr& handle);
    void handleRequestBecomeChild(MessageHandlePtr& handle);
    void handleRejectBecomeChild(MessageHandlePtr& handle);
    void handleAcceptBecomeChild(MessageHandlePtr& handle);
    void handleAckAcceptBecomeChild(MessageHandlePtr& handle);
    void handleSendJob(MessageHandlePtr& handle);
    void handleUpdateVolume(MessageHandlePtr& handle);
    void handleJobCommunication(MessageHandlePtr& handle);
    void handleTerminate(MessageHandlePtr& handle);
    void handleInterrupt(MessageHandlePtr& handle);
    void handleAbort(MessageHandlePtr& handle);
    void handleWorkerFoundResult(MessageHandlePtr& handle);
    void handleQueryJobResult(MessageHandlePtr& handle);
    void handleForwardClientRank(MessageHandlePtr& handle);
    void handleWorkerDefecting(MessageHandlePtr& handle);
    void handleNotifyJobRevision(MessageHandlePtr& handle);
    void handleQueryJobRevisionDetails(MessageHandlePtr& handle);
    void handleSendJobRevisionDetails(MessageHandlePtr& handle);
    void handleAckJobRevisionDetails(MessageHandlePtr& handle);
    void handleSendJobRevisionData(MessageHandlePtr& handle);
    void handleIncrementalJobFinished(MessageHandlePtr& handle);
    void handleExit(MessageHandlePtr& handle);

    void bounceJobRequest(JobRequest& request);
    void informClient(int jobId, int clientRank);
    void updateVolume(int jobId, int demand);
    void initJob(MessageHandlePtr handle);
    void interruptJob(MessageHandlePtr& handle, int jobId, bool terminate, bool reckless);
    
    void rebalance();
    void finishBalancing();
    float reduce(float contribution, int rootRank);
    float allReduce(float contribution);

    int getLoad() const {return load;};
    void setLoad(int load, int whichJobId);
    bool isIdle() const {return load == 0;};
    bool hasJobCommitments() const {return jobCommitments.size() > 0;};
    int getRandomWorkerNode();
    bool isTimeForRebalancing();

    bool hasJob(int id) const {
        return jobs.count(id) > 0;
    }
    Job& getJob(int id) const {
        return *jobs.at(id);
    };

    std::string jobStr(int j, int idx) const {
        return "#" + std::to_string(j) + ":" + std::to_string(idx);
    };

    int maxJobHops(bool rootNode) {
        if (rootNode) {
            return MyMpi::size(comm) / 2;
        }
        return MyMpi::size(comm) * 2;
    }
};

#endif
