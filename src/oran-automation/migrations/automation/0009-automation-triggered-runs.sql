CREATE TABLE IF NOT EXISTS automation_triggered_runs(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  job_key TEXT NOT NULL,
  trigger_key TEXT NOT NULL,
  fired_at TEXT NOT NULL,
  finished_at TEXT NOT NULL,
  success INTEGER NOT NULL CHECK(success IN (0, 1)),
  outcome TEXT NOT NULL DEFAULT 'success' CHECK(outcome IN ('success', 'failure', 'aborted')),
  error_message TEXT,
  created_at TEXT NOT NULL,
  FOREIGN KEY(job_key) REFERENCES automation_triggered_jobs(job_key) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_automation_triggered_runs_job_fired
  ON automation_triggered_runs(job_key, fired_at DESC, id DESC);

CREATE INDEX IF NOT EXISTS idx_automation_triggered_runs_trigger_fired
  ON automation_triggered_runs(trigger_key, fired_at DESC, id DESC);
