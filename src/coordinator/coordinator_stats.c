#include "coordinator_stats.h"

static int collect_jobs(const struct faultline_coordinator_store *store,
                         struct faultline_stats_payload *stats)
{
    if (!store->opened || store->failure != FAULTLINE_STORE_FAILURE_NONE ||
        store->jobs.count > FAULTLINE_JOBS_MAX_ENTRIES) { return -1; }
    stats->jobs_submitted_total = store->jobs.count;
    for (size_t i = 0; i < store->jobs.count; ++i) {
        const struct faultline_job *job = &store->jobs.jobs[i];
        switch (job->state) {
        case FAULTLINE_JOB_QUEUED: ++stats->jobs_queued; break;
        case FAULTLINE_JOB_ASSIGNED: ++stats->jobs_assigned; break;
        case FAULTLINE_JOB_RUNNING: ++stats->jobs_running; break;
        case FAULTLINE_JOB_DONE: ++stats->jobs_completed_total; break;
        case FAULTLINE_JOB_FAILED: ++stats->jobs_failed_total; break;
        default: return -1;
        }
        if (job->retry_count > job->max_retries ||
            job->attempt != (uint64_t)job->retry_count + (job->state == FAULTLINE_JOB_QUEUED ? 0u : 1u)) {
            return -1;
        }
        /* At most 256 entries, each with at most 2^32 attempts. */
        stats->job_attempts_total += job->attempt;
        stats->job_retries_total += job->retry_count;
    }
    uint64_t remainder = 0;
    for (size_t i = 0; i < store->jobs.count; ++i) {
        const struct faultline_job *job = &store->jobs.jobs[i];
        if (job->state != FAULTLINE_JOB_DONE) { continue; }
        if (job->created_at_ms < 0 || job->finished_at_ms < job->created_at_ms) { return -1; }
        uint64_t latency = (uint64_t)(job->finished_at_ms - job->created_at_ms);
        /* Exact floored mean without overflowing a sum of large durations. */
        stats->completed_latency_avg_ms += latency / stats->jobs_completed_total;
        remainder += latency % stats->jobs_completed_total;
    }
    if (stats->jobs_completed_total != 0) {
        stats->completed_latency_avg_ms += remainder / stats->jobs_completed_total;
    }
    return 0;
}

int faultline_stats_begin(struct faultline_stats_session *session,
                          const struct faultline_coordinator_store *store, int64_t now_ms)
{
    struct faultline_stats_payload baseline = {0};
    if (session == NULL || store == NULL || store->session_start_ms < 0 ||
        now_ms < store->session_start_ms || collect_jobs(store, &baseline) < 0 ||
        store->interrupted_jobs > store->jobs.count) { return -1; }
    *session = (struct faultline_stats_session){
        .ready_at_ms = now_ms,
        .startup_duration_ms = (uint64_t)(now_ms - store->session_start_ms),
        .startup_interrupted_jobs = store->interrupted_jobs,
        .submitted_at_start = baseline.jobs_submitted_total,
        .completed_at_start = baseline.jobs_completed_total,
        .failed_at_start = baseline.jobs_failed_total,
        .retries_at_start = baseline.job_retries_total
    };
    return 0;
}

int faultline_stats_snapshot(const struct faultline_stats_session *session,
                             const struct faultline_coordinator_store *store,
                             int64_t now_ms, int heartbeat_timeout_ms,
                             struct faultline_stats_payload *output)
{
    struct faultline_stats_payload stats = {0};
    if (session == NULL || store == NULL || output == NULL || session->ready_at_ms < 0 ||
        now_ms < session->ready_at_ms || heartbeat_timeout_ms <= 0 ||
        collect_jobs(store, &stats) < 0 ||
        stats.jobs_submitted_total < session->submitted_at_start ||
        stats.jobs_completed_total < session->completed_at_start ||
        stats.jobs_failed_total < session->failed_at_start ||
        stats.job_retries_total < session->retries_at_start) { return -1; }
    stats.session_uptime_ms = (uint64_t)(now_ms - session->ready_at_ms);
    stats.session_jobs_submitted = stats.jobs_submitted_total - session->submitted_at_start;
    stats.session_jobs_completed = stats.jobs_completed_total - session->completed_at_start;
    stats.session_jobs_failed = stats.jobs_failed_total - session->failed_at_start;
    stats.session_job_retries = stats.job_retries_total - session->retries_at_start;
    stats.startup_jobs_recovered = session->submitted_at_start;
    stats.startup_interrupted_jobs = session->startup_interrupted_jobs;
    stats.startup_duration_ms = session->startup_duration_ms;
    stats.heartbeat_timeout_ms = (uint64_t)heartbeat_timeout_ms;
    for (size_t i = 0; i < FAULTLINE_MAX_WORKERS; ++i) {
        const struct faultline_worker *worker = &store->workers.workers[i];
        if (worker->state == FAULTLINE_WORKER_UNUSED) { continue; }
        ++stats.workers_retained;
        if (worker->state == FAULTLINE_WORKER_DEAD) { ++stats.workers_dead; continue; }
        if (worker->state != FAULTLINE_WORKER_ALIVE || worker->last_heartbeat_ms < 0 ||
            now_ms < worker->last_heartbeat_ms) { return -1; }
        if (faultline_worker_timed_out(worker, now_ms, heartbeat_timeout_ms)) {
            ++stats.workers_expired;
        } else {
            ++stats.workers_alive;
            if (faultline_scheduler_active(&store->jobs, worker->id) != NULL) { ++stats.workers_busy; }
            else { ++stats.workers_idle; }
        }
    }
    *output = stats;
    return 0;
}
