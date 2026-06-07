CREATE TABLE IF NOT EXISTS automation_cron_leases(
  job_key TEXT PRIMARY KEY,
  owner_key TEXT NOT NULL,
  acquired_at TEXT NOT NULL,
  expires_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  FOREIGN KEY(job_key) REFERENCES automation_cron_jobs(job_key) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_automation_cron_leases_expires
  ON automation_cron_leases(expires_at);
