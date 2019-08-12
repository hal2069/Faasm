
extern "C" {
#include <faasm/core.h>
#include <faasm/host_interface.h>
}

#include <util/locks.h>
#include <util/logging.h>
#include <redis/Redis.h>
#include <state/State.h>
#include <thread>

/**
 * C++ emulation of Faasm system
 */

// We use an empty emulator user by default as it's easier to reason about keys in state
static std::string _user;

// Note thread locality here to handle multiple locally chained functions
static thread_local std::vector<uint8_t> _inputData;
static thread_local int _funcIdx = 0;

static std::mutex threadsMutex;
static std::unordered_map<int, std::thread> threads;
static int threadCount = 1;

#define DUMMY_USER "emulated"

std::string getEmulatorUser() {
    return _user;
}

void setEmulatorUser(const char *newUser) {
    _user = newUser;
}

void unsetEmulatorUser() {
    _user = "";
}

std::shared_ptr<state::StateKeyValue> getKv(const char *key, size_t size) {
    state::State &s = state::getGlobalState();

    if(_user.empty()) {
        const std::shared_ptr<spdlog::logger> &logger = util::getLogger();
        logger->debug("Setting dummy emulator user {}", DUMMY_USER);
        setEmulatorUser(DUMMY_USER);
    }

    return s.getKV(_user, key, size);
}

void __faasm_read_state(const char *key, unsigned char *buffer, long bufferLen, int async) {
    auto kv = getKv(key, bufferLen);

    bool isAsync = async == 1;
    kv->pull(isAsync);

    kv->get(buffer);
}

unsigned char *__faasm_read_state_ptr(const char *key, long totalLen, int async) {
    auto kv = getKv(key, totalLen);

    bool isAsync = async == 1;
    kv->pull(isAsync);

    return kv->get();
}


void __faasm_read_state_offset(const char *key, long totalLen, long offset, unsigned char *buffer, long bufferLen,
                               int async) {
    auto kv = getKv(key, totalLen);

    bool isAsync = async == 1;
    kv->pull(isAsync);

    kv->getSegment(offset, buffer, bufferLen);
}

unsigned char *__faasm_read_state_offset_ptr(const char *key, long totalLen, long offset, long len, int async) {
    auto kv = getKv(key, totalLen);

    bool isAsync = async == 1;
    kv->pull(isAsync);

    return kv->getSegment(offset, len);
}

void __faasm_write_state(const char *key, const uint8_t *data, long dataLen, int async) {
    auto kv = getKv(key, dataLen);
    kv->set(data);

    if (async == 0) {
        kv->pushFull();
    }
}

void __faasm_write_state_offset(const char *key, long totalLen, long offset, const unsigned char *data, long dataLen,
                                int async) {
    auto kv = getKv(key, totalLen);

    kv->setSegment(offset, data, dataLen);

    if (async == 0) {
        kv->pushPartial();
    }
}

void __faasm_flag_state_dirty(const char *key, long totalLen) {
    auto kv = getKv(key, totalLen);
    kv->flagFullValueDirty();
}

void __faasm_flag_state_offset_dirty(const char *key, long totalLen, long offset, long dataLen) {
    auto kv = getKv(key, totalLen);
    kv->flagSegmentDirty(offset, dataLen);
}


void __faasm_push_state(const char *key) {
    auto kv = getKv(key, 0);
    kv->pushFull();
}

void __faasm_push_state_partial(const char *key) {
    auto kv = getKv(key, 0);
    kv->pushPartial();
}

long __faasm_read_input(unsigned char *buffer, long bufferLen) {
    // This relies on thread-local _inputData
    if(bufferLen==0) {
        return _inputData.size();
    }

    std::copy(_inputData.begin(), _inputData.end(), buffer);

    return bufferLen;
}

int __faasm_chain_this(int idx, const unsigned char *buffer, long bufferLen) {
    const std::shared_ptr<spdlog::logger> &logger = util::getLogger();

    // Create vector to hold inputs
    std::vector<uint8_t> inputs(buffer, buffer + bufferLen);

    // Create a fake thread ID for this thread
    int thisCallId;
    {
        util::UniqueLock threadsLock(threadsMutex);

        threadCount++;
        thisCallId = threadCount;

        // Spawn a thread to execute the function
        threads.emplace(std::pair<int, std::thread>(thisCallId, [idx, inputs] {
            // Set up input data for this thread (thread-local)
            _inputData = inputs;
            _funcIdx = idx;

            // Invoke the function
            _FaasmFuncPtr f = getFaasmFunc(idx);
            f();
        }));
    }

    logger->debug("Chained local function {} with call ID {}", idx, thisCallId);
    return thisCallId;
}

int __faasm_await_call(int callId) {
    const std::shared_ptr<spdlog::logger> &logger = util::getLogger();

    // Check this is valid
    if(threads.count(callId) == 0) {
        logger->error("Call with id {} doesn't exist", callId);
        throw std::runtime_error("Awaiting non-existent call");
    }

    // Join the thread to await its completion
    std::thread &t = threads[callId];
    if(t.joinable()) {
        t.join();
    } else {
        logger->error("Call with id {} not joinable", callId);
        throw std::runtime_error("Cannot join thread");
    }

    return 0;
}

int __faasm_get_idx() {
    // Relies on thread-local idx
    return _funcIdx;
}

void __faasm_lock_state_read(const char *key) {

}

void __faasm_unlock_state_read(const char *key) {

}

void __faasm_lock_state_write(const char *key) {

}

void __faasm_unlock_state_write(const char *key) {

}

void __faasm_write_output(const unsigned char *output, long outputLen) {

}

int __faasm_chain_function(const char *name, const unsigned char *inputData, long inputDataSize) {
    // This might not be possible when executing natively
    return 1;
}

void __faasm_read_config(const char *varName, char *buffer) {
    util::SystemConfig &conf = util::getSystemConfig();

    if (strcmp(varName, "FULL_ASYNC") == 0) {
        if (conf.fullAsync == 1) {
            strcpy(buffer, "1");
        } else {
            strcpy(buffer, "0");
        }
    } else if (strcmp(varName, "FULL_SYNC") == 0) {
        if (conf.fullSync == 1) {
            strcpy(buffer, "1");
        } else {
            strcpy(buffer, "0");
        }
    }
}
