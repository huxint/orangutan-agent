CREATE TABLE IF NOT EXISTS automation_memory_retention_jobs(
  job_key TEXT PRIMARY KEY,
  scope_key TEXT NOT NULL,
  forget_after_unused_days INTEGER NOT NULL CHECK(forget_after_unused_days > 0),
  importance_floor REAL NOT NULL CHECK(importance_floor >= 0.0 AND importance_floor <= 1.0),
  max_records_per_scope INTEGER NOT NULL CHECK(max_records_per_scope > 0),
  decay_check_interval_hours INTEGER NOT NULL CHECK(decay_check_interval_hours > 0),
  first_fire_at TEXT NOT NULL,
  last_fired_at TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS automation_memory_retention_runs(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  job_key TEXT NOT NULL,
  fired_at TEXT NOT NULL,
  finished_at TEXT NOT NULL,
  success INTEGER NOT NULL CHECK(success IN (0, 1)),
  shadowed_count INTEGER NOT NULL CHECK(shadowed_count >= 0),
  error_message TEXT,
  created_at TEXT NOT NULL,
  FOREIGN KEY(job_key) REFERENCES automation_memory_retention_jobs(job_key) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_automation_memory_retention_runs_job_fired
  ON automation_memory_retention_runs(job_key, fired_at DESC, id DESC);
