# Execution Plans

This directory holds long-lived plans for complex work.

- Put active work in `active/`.
- Delete finished plans after durable decisions and open work have been absorbed;
  Git keeps the archive.
- Start from `templates/execution-plan.md` (or use `make new-plan SLUG=<slug>`).
- Record deferred cleanup in `tech-debt-tracker.md`.

See [`../PLANS_GUIDE.md`](../PLANS_GUIDE.md) for the convention.
