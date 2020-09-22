#ifndef MSCHICK_BASE_CUBE_SAT_JOB_H
#define MSCHICK_BASE_CUBE_SAT_JOB_H

#include <atomic>
#include <memory>

#include "app/job.hpp"
#include "cube_communicator.hpp"
#include "cube_lib.hpp"

class BaseCubeSatJob : public Job {
   private:
    std::unique_ptr<CubeLib> _lib;

    CubeCommunicator _cube_comm;

    std::atomic_bool _isInitialized{false};

   public:
    BaseCubeSatJob(Parameters& params, int commSize, int worldRank, int jobId);

    bool appl_initialize() override;
    bool appl_doneInitializing() override;
    void appl_updateRole() override;
    void appl_updateDescription(int fromRevision) override;
    void appl_pause() override;
    void appl_unpause() override;
    void appl_interrupt() override;
    void appl_withdraw() override;
    int appl_solveLoop() override;

    bool appl_wantsToBeginCommunication() const override;
    void appl_beginCommunication() override;
    void appl_communicate(int source, JobMessage& msg) override;

    void appl_dumpStats() override;
    bool appl_isDestructible() override;

    int getDemand(int prevVolume, float elapsedTime = Timer::elapsedSeconds()) const override;
};

#endif /* MSCHICK_BASE_CUBE_SAT_JOB_H */