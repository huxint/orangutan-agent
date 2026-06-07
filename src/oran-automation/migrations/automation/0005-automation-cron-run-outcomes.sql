ALTER TABLE automation_cron_runs
  ADD COLUMN outcome TEXT NOT NULL DEFAULT 'failure' CHECK(outcome IN ('success', 'failure', 'aborted'));

UPDATE automation_cron_runs
SET outcome = CASE
  WHEN success = 1 THEN 'success'
  ELSE 'failure'
END;
