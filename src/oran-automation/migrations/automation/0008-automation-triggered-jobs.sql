CREATE TABLE IF NOT EXISTS automation_triggered_jobs(
  job_key TEXT PRIMARY KEY,
  trigger_key TEXT NOT NULL,
  agent_key TEXT NOT NULL DEFAULT 'automation',
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_automation_triggered_jobs_trigger_updated
  ON automation_triggered_jobs(trigger_key, updated_at DESC, job_key ASC);
