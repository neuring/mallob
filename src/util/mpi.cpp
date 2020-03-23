
#include <chrono>
#include <unistd.h>
#include <ctime>
#include <algorithm>

#include "mympi.h"
#include "random.h"
#include "timer.h"
#include "console.h"

#include "util/mpi_monitor.h"

int MyMpi::_max_msg_length;
std::set<MessageHandlePtr> MyMpi::_handles;
std::set<MessageHandlePtr> MyMpi::_sentHandles;
std::set<MessageHandlePtr> MyMpi::_deferred_handles;
std::map<int, int> MyMpi::_tag_priority;

void chkerr(int err) {
    if (err != 0) {
        Console::log(Console::CRIT, "MPI ERROR errcode=%i", err);
        abort();
    }
}

bool MessageHandle::testSent() {
    assert(!selfMessage || Console::fail("Attempting to MPI_Test a self message!"));
    if (finished) return true;
    int flag = 0;
    std::string op = "testsent-id" + std::to_string(id);
    initcall(op.c_str());
    int err = MPI_Test(&request, &flag, &status);
    endcall();
    chkerr(err);
    if (flag) finished = true;
    return finished;
}

bool MessageHandle::testReceived() {

    // Already finished at some earlier point?
    if (finished) return true;

    // Check if message was received
    if (selfMessage || status.MPI_TAG > 0) {
        // Self message / fabricated message is invariantly ready to be processed
        finished = true;
    } else {
        // MPI_Test message
        int flag = -1;
        std::string op = "testrecvd-id" + std::to_string(id);
        initcall(op.c_str());
        int err = MPI_Test(&request, &flag, &status);
        endcall();
        chkerr(err);
        if (flag) {
            finished = true;
            tag = status.MPI_TAG;
            assert(status.MPI_SOURCE >= 0 || Console::fail("MPI_SOURCE = %i", status.MPI_SOURCE));
            source = status.MPI_SOURCE;
        }
    }

    // Resize received data vector to actual received size
    if (finished && !selfMessage) {
        int count = 0;
        std::string op = "getCount-id" + std::to_string(id);
        initcall(op.c_str());
        int err = MPI_Get_count(&status, MPI_BYTE, &count);
        endcall();
        chkerr(err);
        if (count > 0 && count < recvData->size()) {
            recvData->resize(count);
        }
    }

    return finished;
}


int MyMpi::nextHandleId() {
    return handleId++;
}

void MyMpi::init(int argc, char *argv[])
{
    initcall("init");
    
    int provided = -1;
    int err = MPI_Init_thread(&argc, &argv, MPI_THREAD_SINGLE, &provided);
    chkerr(err);
    if (provided != MPI_THREAD_SINGLE) {
        std::cout << "ERROR initializing MPI: wanted id=" << MPI_THREAD_SINGLE 
                << ", got id=" << provided << std::endl;
        exit(1);
    }

    //int err = MPI_Init(&argc, &argv);
    //chkerr(err);
    
    endcall();

    _max_msg_length = MyMpi::size(MPI_COMM_WORLD) * MAX_JOB_MESSAGE_PAYLOAD_PER_NODE + 10;
    handleId = 1;

    // Very quick messages, and such which are critical for overall system performance
    _tag_priority[MSG_FIND_NODE] = 0;

    _tag_priority[MSG_WORKER_FOUND_RESULT] = 1;
    _tag_priority[MSG_FORWARD_CLIENT_RANK] = 1;
    _tag_priority[MSG_JOB_DONE] = 1;
    _tag_priority[MSG_COLLECTIVES] = 1;
    _tag_priority[MSG_ANYTIME_REDUCTION] = 1;
    _tag_priority[MSG_ANYTIME_BROADCAST] = 1;
    
    // Job-internal management messages
    _tag_priority[MSG_REQUEST_BECOME_CHILD] = 2;
    _tag_priority[MSG_ACCEPT_BECOME_CHILD] = 2;
    _tag_priority[MSG_REJECT_BECOME_CHILD] = 2;
    _tag_priority[MSG_QUERY_VOLUME] = 2;
    _tag_priority[MSG_UPDATE_VOLUME] = 2;
    _tag_priority[MSG_NOTIFY_JOB_REVISION] = 2;
    _tag_priority[MSG_QUERY_JOB_REVISION_DETAILS] = 2;
    _tag_priority[MSG_SEND_JOB_REVISION_DETAILS] = 2;
    _tag_priority[MSG_ACK_JOB_REVISION_DETAILS] = 2;

    // Messages inducing bulky amounts of work
    _tag_priority[MSG_ACK_ACCEPT_BECOME_CHILD] = 3; // sending a job desc.
    _tag_priority[MSG_SEND_JOB_DESCRIPTION] = 3; // receiving a job desc.
    _tag_priority[MSG_QUERY_JOB_RESULT] = 3; // sending a job result
    _tag_priority[MSG_SEND_JOB_RESULT] = 3; // receiving a job result

    // Termination and interruption
    _tag_priority[MSG_TERMINATE] = 4;
    _tag_priority[MSG_INTERRUPT] = 4;
    _tag_priority[MSG_ABORT] = 4;
    _tag_priority[MSG_WORKER_DEFECTING] = 4;

    // Job-specific communication: not critical for balancing 
    _tag_priority[MSG_JOB_COMMUNICATION] = 5;

    _tag_priority[MSG_WARMUP] = 6;
}

void MyMpi::beginListening(const ListenerMode& mode) {
    if (mode == CLIENT) {
        for (int tag : ANYTIME_CLIENT_RECV_TAGS) {
            MessageHandlePtr handle = MyMpi::irecv(MPI_COMM_WORLD, tag);
            Console::log(Console::VVVERB, "Msg ID=%i : listening to tag %i", handle->id, tag);
        }
    }
    if (mode == WORKER) {
        for (int tag : ANYTIME_WORKER_RECV_TAGS) {
            MessageHandlePtr handle = MyMpi::irecv(MPI_COMM_WORLD, tag);
            Console::log(Console::VVVERB, "Msg ID=%i : listening to tag %i", handle->id, tag);
        }
    }
}

void MyMpi::resetListenerIfNecessary(const ListenerMode& mode, int tag) {
    // Check if tag should be listened to
    if (!isAnytimeTag(mode, tag)) return;
    // add listener
    MessageHandlePtr handle = MyMpi::irecv(MPI_COMM_WORLD, tag);
    Console::log(Console::VVVERB, "Msg ID=%i : listening to tag %i", handle->id, tag);
}

bool MyMpi::isAnytimeTag(const ListenerMode& mode, int tag) {
    if (mode == CLIENT)
        for (int t : ANYTIME_CLIENT_RECV_TAGS)
            if (tag == t) return true;
    if (mode == WORKER) 
        for (int t : ANYTIME_WORKER_RECV_TAGS)
            if (tag == t) return true;
    return false;
}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const Serializable& object) {
    
    float time = Timer::elapsedSeconds();
    std::shared_ptr<std::vector<uint8_t>> vec = object.serialize();
    float timeSerialize = Timer::elapsedSeconds() - time;

    time = Timer::elapsedSeconds();
    MessageHandlePtr handle = isend(communicator, recvRank, tag, vec);
    float timeSend = Timer::elapsedSeconds() - time;

    Console::log(Console::VVVERB, "Msg ID=%i serializeTime=%.4f totalIsendTime=%.4f", handle->id, timeSerialize, timeSend);
    return handle;
}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const std::shared_ptr<std::vector<uint8_t>>& object) {

    if (object->empty()) {
        object->push_back(0);
    }
    MessageHandlePtr handle(new MessageHandle(nextHandleId(), object));

    bool selfMessage = rank(communicator) == recvRank;
    if (selfMessage) {
        handle->recvData = handle->sendData;
        handle->tag = tag;
        handle->source = recvRank;
        handle->selfMessage = true;
    } else {
        initcall("isend");
        int err = MPI_Isend(handle->sendData->data(), handle->sendData->size(), MPI_BYTE, recvRank, tag, communicator, &handle->request);
        endcall();
        chkerr(err);
    }

    (selfMessage ? _handles : _sentHandles).insert(handle);
    
    Console::log(Console::VVVERB, "Msg ID=%i dest=%i tag=%i size=%i", handle->id, 
                recvRank, tag, handle->sendData->size());
    return handle;
}

MessageHandlePtr MyMpi::send(MPI_Comm communicator, int recvRank, int tag, const Serializable& object) {
    return send(communicator, recvRank, tag, object.serialize());
}

MessageHandlePtr MyMpi::send(MPI_Comm communicator, int recvRank, int tag, const std::shared_ptr<std::vector<uint8_t>>& object) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId(), object));
    if (rank(communicator) == recvRank) {
        handle->recvData = handle->sendData;
        handle->tag = tag;
        handle->source = recvRank;
        handle->selfMessage = true;
        _handles.insert(handle);
    } else {
        initcall("send");
        int err = MPI_Send(handle->sendData->data(), handle->sendData->size(), MPI_BYTE, recvRank, tag, communicator);
        endcall();
        chkerr(err);
    }
    Console::log(Console::VVVERB, "Msg ID=%i sent.", handle->id);
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator) {
    return irecv(communicator, MPI_ANY_TAG);
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int tag) {
    return irecv(communicator, MPI_ANY_SOURCE, tag);
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int source, int tag) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    handle->tag = tag;

    int msgSize;
    if (tag == MSG_JOB_COMMUNICATION || tag == MSG_COLLECTIVES || tag == MPI_ANY_TAG) {
        msgSize = _max_msg_length;
    } else {
        msgSize = MAX_ANYTIME_MESSAGE_SIZE;
    }

    handle->recvData->resize(msgSize);
    initcall("irecv");
    int err = MPI_Irecv(handle->recvData->data(), msgSize, MPI_BYTE, source, tag, communicator, &handle->request);
    endcall();
    chkerr(err);
    _handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int source, int tag, int size) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    assert(source >= 0);
    handle->source = source;
    handle->tag = tag;
    handle->recvData->resize(size);
    initcall("irecv");
    int err = MPI_Irecv(handle->recvData->data(), size, MPI_BYTE, source, tag, communicator, &handle->request);
    endcall();
    chkerr(err);
    _handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::recv(MPI_Comm communicator, int tag, int size) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    handle->tag = tag;
    handle->recvData->resize(size);
    initcall("recv");
    int err = MPI_Recv(handle->recvData->data(), size, MPI_BYTE, MPI_ANY_SOURCE, tag, communicator, &handle->status);
    endcall();
    chkerr(err);
    handle->source = handle->status.MPI_SOURCE;
    int count = 0;
    err = MPI_Get_count(&handle->status, MPI_BYTE, &count);
    chkerr(err);
    if (count < size) {
        handle->recvData->resize(count);
    }
    return handle;
}

MessageHandlePtr MyMpi::recv(MPI_Comm communicator, int tag) {
    return recv(communicator, tag, _max_msg_length);
}

MPI_Request MyMpi::iallreduce(MPI_Comm communicator, float* contribution, float* result) {
    MPI_Request req;
    initcall("iallreduce");
    MPI_Iallreduce(contribution, result, 1, MPI_FLOAT, MPI_SUM, communicator, &req);
    endcall();
    return req;
}

MPI_Request MyMpi::iallreduce(MPI_Comm communicator, float* contribution, float* result, int numFloats) {
    MPI_Request req;
    initcall("iallreduce");
    MPI_Iallreduce(contribution, result, numFloats, MPI_FLOAT, MPI_SUM, communicator, &req);
    endcall();
    return req;
}

MPI_Request MyMpi::ireduce(MPI_Comm communicator, float* contribution, float* result, int rootRank) {
    MPI_Request req;
    initcall("ireduce");
    MPI_Ireduce(contribution, result, 1, MPI_FLOAT, MPI_SUM, rootRank, communicator, &req);
    endcall();
    return req;
}

bool MyMpi::test(MPI_Request& request, MPI_Status& status) {
    int flag = 0;
    initcall("test");
    int err = MPI_Test(&request, &flag, &status);
    chkerr(err);
    endcall();
    return flag;
}

MessageHandlePtr MyMpi::poll(const ListenerMode& mode) {

    MessageHandlePtr bestPrioHandle = NULL;
    int bestPrio = 9999999;

    // Find ready handle of best priority
    for (auto h : _handles) {
        if (h->testReceived() && _tag_priority[h->tag] < bestPrio) {
            bestPrio = _tag_priority[h->tag];
            bestPrioHandle = h;
            if (bestPrio == MIN_PRIORITY) break; // handle of minimum priority found
        }
    }

    // If necessary, pick a deferred handle (if there is one)
    if (bestPrioHandle == NULL) {
        for (auto h : _deferred_handles) {
            if (h->testReceived() && _tag_priority[h->tag] < bestPrio) {
                bestPrio = _tag_priority[h->tag];
                bestPrioHandle = h;
                if (bestPrio == MIN_PRIORITY) break; // handle of minimum priority found
            }
        }   
    }

    // Remove and return found handle
    if (bestPrioHandle != NULL) {
        _handles.erase(bestPrioHandle);
        resetListenerIfNecessary(mode, bestPrioHandle->tag);
    } 
    return bestPrioHandle;
}

void MyMpi::deferHandle(MessageHandlePtr handle) {
    _deferred_handles.insert(handle);
}

bool MyMpi::hasOpenSentHandles() {
    return !_sentHandles.empty();
}

void MyMpi::testSentHandles() {
    std::set<MessageHandlePtr> finishedHandles;
    for (auto h : _sentHandles) {
        if (h->testSent()) {
            // Sending operation completed
            Console::log(Console::VVVERB, "Msg ID=%i isent", h->id);
            finishedHandles.insert(h);
        }
    }
    for (auto h : finishedHandles) {
        _sentHandles.erase(h);
    }
}

int MyMpi::size(MPI_Comm comm) {
    int size = 0;
    initcall("commSize");
    int err = MPI_Comm_size(comm, &size);
    endcall();
    chkerr(err);
    return size;
}

int MyMpi::rank(MPI_Comm comm) {
    int rank = -1;
    initcall("commRank");
    int err = MPI_Comm_rank(comm, &rank);
    endcall();
    chkerr(err);
    return rank;
}

int MyMpi::random_other_node(MPI_Comm comm, const std::set<int>& excludedNodes)
{
    int size = MyMpi::size(comm);

    float r = Random::rand();
    int node = (int) (r * size);

    while (excludedNodes.find(node) != excludedNodes.end()) {
        r = Random::rand();
        node = (int) (r * size);
    }

    return node;
}
