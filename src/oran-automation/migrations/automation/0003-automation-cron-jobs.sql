CREATE TABLE IF NOT EXISTS automation_cron_jobs(
  job_key TEXT PRIMARY KEY,
  expression TEXT NOT NULL,
  first_fire_at TEXT NOT NULL,
  last_fired_at TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_automation_cron_jobs_updated
  ON automation_cron_jobs(updated_at DESC, job_key ASC);
