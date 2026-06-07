CREATE TABLE IF NOT EXISTS automation_memory_retention_leases(
  job_key TEXT PRIMARY KEY,
  owner_key TEXT NOT NULL,
  acquired_at TEXT NOT NULL,
  expires_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  FOREIGN KEY(job_key) REFERENCES automation_memory_retention_jobs(job_key) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_automation_memory_retention_leases_expires
  ON automation_memory_retention_leases(expires_at);
