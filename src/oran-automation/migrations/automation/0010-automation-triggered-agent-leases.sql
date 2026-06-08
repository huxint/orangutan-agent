CREATE TABLE IF NOT EXISTS automation_triggered_agent_leases(
  agent_key TEXT PRIMARY KEY,
  owner_key TEXT NOT NULL,
  acquired_at TEXT NOT NULL,
  expires_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_automation_triggered_agent_leases_expires
  ON automation_triggered_agent_leases(expires_at);
