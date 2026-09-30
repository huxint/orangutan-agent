# Orangutan Desktop Prototype

A browser-based desktop interface for a local general-purpose assistant. Five
pages cover conversations, tasks, connections, memory and settings. Chinese and
English, light/dark/system themes, a shared workspace and per-requester memory
are included. The [product specification](../../docs/product-specs/desktop-prototype.md)
owns behavior and the boundary between local actions and read-only channel views.

This is a demo: model replies, agent teams, file results and Telegram/QQ/Feishu
connections are simulated. It sends no messages, calls no provider, reads no
local workspace and stores no credentials. Nothing runs after the page closes.

## Run

Use Node.js 24 or later and npm. From the repository root:

```sh
npm --prefix apps/desktop ci
npm --prefix apps/desktop run dev
```

Open <http://127.0.0.1:5173>. The server binds to loopback and requires that port
to be free. This optional app does not participate in the C++/xmake build.

## Explore

- Start with a suggestion or type a request in the desktop conversation.
  File/organization requests demonstrate approvals; plan/research requests
  demonstrate a small team. A request mentioning `失败` or `error` demonstrates
  a failed run that can be retried. Other requests show a conversational reply.
- Say `记住…` / `remember…`, or express a preference such as `我喜欢…` / `I prefer…`,
  to demonstrate a personal memory entry when auto memory is enabled.
  The Memory page supports editing, scoped search and deletion.
- Channel conversations are previews. There is no composer, approval or task
  control in them, including the task detail panel. Notifications link back to
  the original conversation without sending anything.
- In Connections, configure a sample account and choose a successful or failed
  demo connection. Connecting the second QQ account starts its queued team example.
- Use `Ctrl+K` (or `Cmd+K`) to search conversations and tasks. Dialogs support Escape
  and restore keyboard focus. The right panel replaces the main content at narrow widths.

## State And Recovery

The page owns one demo scheduler. State is saved under the browser storage key
`orangutan.desktop.demo`. Reopening marks active attempts interrupted, expires
pending approvals and disconnects the simulated transports. There is no automatic
replay, cross-tab agent service or hidden background work. Explicitly reconnect
channels or retry local work as needed.

Malformed/unreadable state is preserved and automatic writes are blocked; a
banner offers export and recovery. Settings can export the original snapshot or
restart the demo. A reset backs up the previous value under
`orangutan.desktop.demo.backup.<timestamp>` before replacement. A failed backup
or write does not claim success. These records contain only the demo data and
text entered into this browser, not existing Orangutan runtime databases.

## Verify

```sh
npm --prefix apps/desktop run build
npm --prefix apps/desktop run format:check
npm --prefix apps/desktop test
cd apps/desktop
npx playwright install chromium
npm run test:e2e
```

Run `make ci` from the repository root as well. Node tests cover state boundaries,
approvals, event attribution, notifications, memory and recovery. Browser tests
exercise actual controls, source restrictions, persistence, themes, languages,
keyboard focus and page lifecycle. Screenshots and failed traces are written to
the ignored `test-results/` directory.

An existing compatible Chromium can be used without another browser download:
set `ORAN_BROWSER_PATH` to its executable when running `npm run test:e2e`.
Playwright starts the development server when needed, or reuses port 5173.

## Code Boundaries

| Files | Responsibility |
| --- | --- |
| `schema.ts`, `model.ts` | Validated identities, pure user commands and separately applied demo observations. |
| `fixtures.ts` | Example conversations, work, artifacts and scoped memories. |
| `storage.ts`, `useAgent.ts` | Snapshot effects, safe recovery and page-owned scheduling. |
| `App.tsx`, `context.ts` | Navigation, theme/language, notifications and UI composition. |
| `Assistant.tsx`, `Pages.tsx`, `TaskPanel.tsx` | Five pages and artifact/progress/team views. |
| `components.tsx`, `RichText.tsx`, `styles.css` | Shared controls, safe lazy-loaded Markdown and visual system. |

The application imports no C++ runtime and exposes no live-host API. A future
runtime adapter needs its own bounded integration plan and must preserve the
read-only channel boundary; a frontend check is not a server authorization check.
