CREATE TABLE IF NOT EXISTS automation_cron_runs(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  job_key TEXT NOT NULL,
  fired_at TEXT NOT NULL,
  finished_at TEXT NOT NULL,
  success INTEGER NOT NULL CHECK(success IN (0, 1)),
  error_message TEXT,
  created_at TEXT NOT NULL,
  FOREIGN KEY(job_key) REFERENCES automation_cron_jobs(job_key) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_automation_cron_runs_job_fired
  ON automation_cron_runs(job_key, fired_at DESC, id DESC);
