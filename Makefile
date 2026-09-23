CC ?= cc
PYTHON ?= python3
INTEGRATION_ARGS ?=
SANITIZE ?= 0

CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O0 -g
PROJECT_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Wshadow \
	-Wconversion -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
SANITIZER_FLAGS :=

ifeq ($(SANITIZE),1)
BUILD_DIR := build/sanitize
SANITIZER_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer \
	-fno-sanitize-recover=all
else
BUILD_DIR := build/debug
endif

COMMON_SOURCES := $(wildcard src/common/*.c)
COMMON_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(COMMON_SOURCES))
REGISTRY_OBJECT := $(BUILD_DIR)/coordinator/worker_registry.o
JOB_OBJECT := $(BUILD_DIR)/coordinator/job.o
QUEUE_OBJECT := $(BUILD_DIR)/coordinator/job_queue.o
SCHEDULER_OBJECT := $(BUILD_DIR)/coordinator/scheduler.o
WAL_FORMAT_OBJECT := $(BUILD_DIR)/coordinator/wal_format.o
WAL_WRITER_OBJECT := $(BUILD_DIR)/coordinator/wal_writer.o
WAL_REPLAY_OBJECT := $(BUILD_DIR)/coordinator/wal_replay.o
STORE_OBJECT := $(BUILD_DIR)/coordinator/coordinator_store.o
COORDINATOR_OBJECTS := $(COMMON_OBJECTS) $(REGISTRY_OBJECT) $(JOB_OBJECT) $(QUEUE_OBJECT) $(SCHEDULER_OBJECT) $(WAL_FORMAT_OBJECT) $(WAL_WRITER_OBJECT) $(WAL_REPLAY_OBJECT) $(STORE_OBJECT)
CRASH_TEST_OBJECTS := $(BUILD_DIR)/tests/crash_coordinator_main.o $(BUILD_DIR)/tests/crash_coordinator_io.o
TASK_OBJECT := $(BUILD_DIR)/worker/task.o
MAIN_OBJECTS := $(BUILD_DIR)/coordinator/main.o \
	$(BUILD_DIR)/worker/main.o $(BUILD_DIR)/cli/main.o
TEST_NAMES := test_protocol test_messages test_net test_worker_registry test_jobs test_job_queue test_job_messages test_scheduler test_tasks test_wal test_wal_writer test_wal_replay test_coordinator_store
TEST_OBJECTS := $(addprefix $(BUILD_DIR)/tests/,$(addsuffix .o,$(TEST_NAMES)))
OBJECTS := $(COMMON_OBJECTS) $(MAIN_OBJECTS) $(TEST_OBJECTS) $(REGISTRY_OBJECT) $(JOB_OBJECT) $(QUEUE_OBJECT) $(SCHEDULER_OBJECT) $(TASK_OBJECT) $(WAL_FORMAT_OBJECT) $(WAL_WRITER_OBJECT) $(WAL_REPLAY_OBJECT) $(STORE_OBJECT)
PROGRAMS := $(BUILD_DIR)/faultline-coordinator \
	$(BUILD_DIR)/faultline-worker $(BUILD_DIR)/faultline
TEST_PROGRAMS := $(addprefix $(BUILD_DIR)/tests/,$(TEST_NAMES))

.PHONY: all sanitize test test-unit test-integration test-failures test-scheduling test-execution test-recovery test-wal test-wal-writer test-wal-replay test-persistence test-startup-recovery test-coordinator-crashes test-sanitize clean

all: $(PROGRAMS)

sanitize:
	$(MAKE) SANITIZE=1 all

test: test-unit test-integration

test-unit: $(TEST_PROGRAMS)
	./$(BUILD_DIR)/tests/test_protocol
	./$(BUILD_DIR)/tests/test_messages
	./$(BUILD_DIR)/tests/test_net
	./$(BUILD_DIR)/tests/test_worker_registry
	./$(BUILD_DIR)/tests/test_jobs
	./$(BUILD_DIR)/tests/test_job_queue
	./$(BUILD_DIR)/tests/test_job_messages
	./$(BUILD_DIR)/tests/test_scheduler
	./$(BUILD_DIR)/tests/test_tasks
	./$(BUILD_DIR)/tests/test_wal
	./$(BUILD_DIR)/tests/test_wal_writer
	./$(BUILD_DIR)/tests/test_wal_replay
	./$(BUILD_DIR)/tests/test_coordinator_store

test-wal: $(BUILD_DIR)/tests/test_wal
	./$(BUILD_DIR)/tests/test_wal

test-wal-writer: $(BUILD_DIR)/tests/test_wal_writer
	./$(BUILD_DIR)/tests/test_wal_writer

test-wal-replay: $(BUILD_DIR)/tests/test_wal_replay
	./$(BUILD_DIR)/tests/test_wal_replay

test-persistence: all $(BUILD_DIR)/tests/test_coordinator_store $(BUILD_DIR)/tests/crash-coordinator
	./$(BUILD_DIR)/tests/test_coordinator_store
	$(PYTHON) tests/integration/test_persistence.py --bin-dir $(BUILD_DIR)
	$(PYTHON) tests/integration/test_startup_recovery.py --bin-dir $(BUILD_DIR)
	$(PYTHON) tests/integration/test_coordinator_crashes.py --bin-dir $(BUILD_DIR)

test-startup-recovery: all $(BUILD_DIR)/tests/test_coordinator_store
	./$(BUILD_DIR)/tests/test_coordinator_store
	$(PYTHON) tests/integration/test_startup_recovery.py --bin-dir $(BUILD_DIR)

test-coordinator-crashes: all $(BUILD_DIR)/tests/crash-coordinator
	$(PYTHON) tests/integration/test_coordinator_crashes.py --bin-dir $(BUILD_DIR)

test-integration: all $(BUILD_DIR)/tests/crash-coordinator
	$(PYTHON) tests/integration/test_persistence.py --bin-dir $(BUILD_DIR)
	$(PYTHON) tests/integration/test_startup_recovery.py --bin-dir $(BUILD_DIR)
	$(PYTHON) tests/integration/test_coordinator_crashes.py --bin-dir $(BUILD_DIR)
	$(PYTHON) tests/integration/test_recovery.py --bin-dir $(BUILD_DIR) $(INTEGRATION_ARGS)
	$(PYTHON) tests/integration/test_execution.py --bin-dir $(BUILD_DIR) $(INTEGRATION_ARGS)
	$(PYTHON) tests/integration/test_failure_detection.py --bin-dir $(BUILD_DIR) $(INTEGRATION_ARGS)
	$(PYTHON) tests/integration/test_scheduling.py --bin-dir $(BUILD_DIR) $(INTEGRATION_ARGS)
	$(PYTHON) tests/integration/test_ping.py --bin-dir $(BUILD_DIR) $(INTEGRATION_ARGS)
	$(PYTHON) tests/integration/test_worker.py --bin-dir $(BUILD_DIR) $(INTEGRATION_ARGS)
	$(PYTHON) tests/integration/test_heartbeat.py --bin-dir $(BUILD_DIR) $(INTEGRATION_ARGS)

test-scheduling: all
	$(PYTHON) tests/integration/test_scheduling.py --bin-dir $(BUILD_DIR) $(INTEGRATION_ARGS)

test-execution: all
	$(PYTHON) tests/integration/test_execution.py --bin-dir $(BUILD_DIR) $(INTEGRATION_ARGS)

test-recovery: all
	$(PYTHON) tests/integration/test_recovery.py --bin-dir $(BUILD_DIR) $(INTEGRATION_ARGS)

test-failures: all
	$(PYTHON) tests/integration/test_failure_detection.py --bin-dir $(BUILD_DIR) $(INTEGRATION_ARGS)

test-sanitize:
	$(MAKE) SANITIZE=1 test

$(BUILD_DIR)/faultline-coordinator: $(BUILD_DIR)/coordinator/main.o $(COORDINATOR_OBJECTS)
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/tests/crash-coordinator: $(CRASH_TEST_OBJECTS) $(COORDINATOR_OBJECTS)
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/tests/crash_coordinator_main.o: src/coordinator/main.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) -Dfaultline_store_open=faultline_test_store_open -MMD -MP -c $< -o $@

$(BUILD_DIR)/faultline-worker: $(BUILD_DIR)/worker/main.o $(COMMON_OBJECTS) $(TASK_OBJECT)
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/faultline: $(BUILD_DIR)/cli/main.o $(COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(TEST_PROGRAMS): $(BUILD_DIR)/tests/%: $(BUILD_DIR)/tests/%.o $(COMMON_OBJECTS)
	$(CC) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/tests/test_worker_registry: $(REGISTRY_OBJECT)

$(BUILD_DIR)/tests/test_jobs $(BUILD_DIR)/tests/test_job_messages: $(JOB_OBJECT)

$(BUILD_DIR)/tests/test_job_queue: $(JOB_OBJECT) $(QUEUE_OBJECT)

$(BUILD_DIR)/tests/test_scheduler: $(JOB_OBJECT) $(QUEUE_OBJECT) $(SCHEDULER_OBJECT)

$(BUILD_DIR)/tests/test_tasks: $(TASK_OBJECT)

$(BUILD_DIR)/tests/test_wal: $(JOB_OBJECT) $(WAL_FORMAT_OBJECT)

$(BUILD_DIR)/tests/test_wal_writer: $(JOB_OBJECT) $(WAL_FORMAT_OBJECT) $(WAL_WRITER_OBJECT)

$(BUILD_DIR)/tests/test_wal_replay: $(JOB_OBJECT) $(QUEUE_OBJECT) $(SCHEDULER_OBJECT) $(REGISTRY_OBJECT) $(WAL_FORMAT_OBJECT) $(WAL_WRITER_OBJECT) $(WAL_REPLAY_OBJECT)

$(BUILD_DIR)/tests/test_coordinator_store: $(JOB_OBJECT) $(QUEUE_OBJECT) $(SCHEDULER_OBJECT) $(REGISTRY_OBJECT) $(WAL_FORMAT_OBJECT) $(WAL_WRITER_OBJECT) $(WAL_REPLAY_OBJECT) $(STORE_OBJECT)

$(BUILD_DIR)/worker/main.o $(BUILD_DIR)/tests/test_tasks.o: PROJECT_CFLAGS += -pthread
$(BUILD_DIR)/faultline-worker $(BUILD_DIR)/tests/test_tasks: LDLIBS += -pthread

$(BUILD_DIR)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PROJECT_CFLAGS) $(SANITIZER_FLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf build

-include $(OBJECTS:.o=.d)
-include $(CRASH_TEST_OBJECTS:.o=.d)
