import { useState } from 'react';
import {
  ArrowDownToLine,
  ArrowRight,
  Cable,
  Check,
  ChevronRight,
  FileText,
  FolderOpen,
  Globe2,
  Leaf,
  LockKeyhole,
  Monitor,
  Moon,
  Pencil,
  Search,
  Settings2,
  ShieldCheck,
  SlidersHorizontal,
  Sun,
  Trash2,
  Users,
  Wifi,
  WifiOff,
} from 'lucide-react';
import { useApp } from './context.ts';
import { active } from './model.ts';
import { Empty, Modal, PlatformIcon, Source, StatusBadge } from './components.tsx';
import type { Connection, Note } from './schema.ts';

function PageHeading({
  eyebrow,
  title,
  detail,
  children,
}: {
  eyebrow: string;
  title: string;
  detail: string;
  children?: React.ReactNode;
}) {
  return (
    <div className="page-heading">
      <div>
        <div className="eyebrow">{eyebrow}</div>
        <h1>{title}</h1>
        <p>{detail}</p>
      </div>
      {children}
    </div>
  );
}

export function TasksPage() {
  const { state, t, openTask } = useApp();
  const [filter, setFilter] = useState('all');
  const [source, setSource] = useState('all');
  const [query, setQuery] = useState('');
  const tasks = state.tasks.filter(
    (task) =>
      (filter === 'all' ||
        (filter === 'active'
          ? active(task.status)
          : filter === 'completed'
            ? task.status === 'completed'
            : ['failed', 'interrupted', 'cancelled'].includes(task.status))) &&
      (source === 'all' ||
        (source === 'desktop'
          ? state.conversations.find((c) => c.id === task.conversationId)?.origin === 'desktop'
          : state.conversations.find((c) => c.id === task.conversationId)?.origin === 'channel')) &&
      task.title.toLocaleLowerCase().includes(query.toLocaleLowerCase()),
  );
  return (
    <div className="page-scroll">
      <PageHeading
        eyebrow={t('一步一步，都算数', 'ONE THING AT A TIME')}
        title={t('每件事，都有着落。', 'A place for every little task.')}
        detail={t(
          '看看正在推进的事，也回顾已经完成的工作。',
          'Follow what’s moving, and come back to what’s done.',
        )}
      />
      <div className="filter-toolbar">
        <div className="segmented">
          {[
            ['all', t('全部', 'All')],
            ['active', t('进行中', 'In progress')],
            ['completed', t('已完成', 'Completed')],
            ['other', t('需关注', 'Needs attention')],
          ].map(([value, label]) => (
            <button
              key={value}
              className={filter === value ? 'active' : ''}
              onClick={() => setFilter(value)}
            >
              {label}
              {value === 'all' && <span>{state.tasks.length}</span>}
            </button>
          ))}
        </div>
        <select
          aria-label={t('任务来源', 'Task source')}
          value={source}
          onChange={(e) => setSource(e.target.value)}
        >
          <option value="all">{t('所有入口', 'All sources')}</option>
          <option value="desktop">{t('桌面', 'Desktop')}</option>
          <option value="channel">{t('消息渠道', 'Channels')}</option>
        </select>
      </div>
      <label className="search-field task-search">
        <Search size={16} />
        <input
          aria-label={t('搜索任务', 'Search tasks')}
          placeholder={t('找一件事…', 'Find a task…')}
          value={query}
          onChange={(e) => setQuery(e.target.value)}
        />
      </label>
      <div className="task-table">
        <div className="task-table-heading">
          <span>{t('任务', 'Task')}</span>
          <span>{t('状态', 'Status')}</span>
          <span>{t('来源', 'Source')}</span>
          <span />
        </div>
        {tasks.map((task) => (
          <button
            className="task-row"
            key={task.id}
            onClick={() => openTask(task.id)}
            data-testid={`task-row-${task.id}`}
          >
            <div className="task-row-main">
              <span className={`activity-icon ${task.kind === 'team' ? 'green' : ''}`}>
                {task.kind === 'team' ? <Users size={18} /> : <FileText size={18} />}
              </span>
              <div>
                <strong>{task.title}</strong>
                <small>
                  {task.members.length
                    ? `${task.members.length} ${t('位协作成员', 'teammates')}`
                    : t('Orangutan 正在照看', 'In good hands with Orangutan')}
                </small>
              </div>
            </div>
            <StatusBadge status={task.status} />
            <Source conversationId={task.conversationId} />
            <ChevronRight size={16} />
          </button>
        ))}
      </div>
      {!tasks.length && (
        <Empty
          title={t('这里暂时很安静', 'A little quiet here')}
          detail={t(
            '换个筛选条件，或者在对话中交代一件新事情。',
            'Try a different filter, or start something in a conversation.',
          )}
        />
      )}
      <div className="page-note">
        <LockKeyhole size={14} />
        {t(
          '来自其他渠道的任务可以查看，操作请回到原渠道。',
          'Channel tasks are here to view. Take action in their original channel.',
        )}
      </div>
    </div>
  );
}

export function ConnectionsPage() {
  const { state, dispatch, t, selectConversation } = useApp();
  const [editing, setEditing] = useState<Connection | null>(null);
  const [account, setAccount] = useState('');
  const [simulateFailure, setSimulateFailure] = useState(false);
  const labels = {
    connected: t('已连接', 'Connected'),
    connecting: t('连接中', 'Connecting'),
    disconnected: t('未连接', 'Disconnected'),
    error: t('连接异常', 'Connection error'),
  };
  return (
    <div className="page-scroll">
      <PageHeading
        eyebrow={t('从你习惯的地方，找到我', 'REACH ME WHERE YOU ARE')}
        title={t('不止一种联系的方式。', 'A familiar way to reach me.')}
        detail={t(
          '消息从哪里来，回复就回到哪里。每段对话都有自己的空间。',
          'Replies go back to where they started. Every conversation has its own space.',
        )}
      />
      <div className="connections-grid">
        {state.connections.map((connection) => (
          <article className="connection-card" key={connection.id}>
            <div className="connection-top">
              <PlatformIcon platform={connection.platform} size={25} />
              <span className={`connection-status ${connection.status}`}>
                <span />
                {labels[connection.status]}
              </span>
            </div>
            <h2>{connection.name}</h2>
            <p className="account-name">{connection.account}</p>
            <div className="connection-meta">
              <span>
                {state.conversations.filter((c) => c.connectionId === connection.id).length}{' '}
                {t('段独立会话', 'separate conversations')}
              </span>
              <span>{t('演示连接', 'Demo connection')}</span>
            </div>
            <div className="connection-actions">
              <button
                className="button secondary"
                onClick={() => {
                  setEditing(connection);
                  setAccount(connection.account);
                  setSimulateFailure(connection.simulateFailure);
                }}
              >
                <Settings2 size={14} />
                {t('配置', 'Configure')}
              </button>
              {connection.status === 'connected' || connection.status === 'connecting' ? (
                <button
                  className="text-button"
                  onClick={() => dispatch({ type: 'disconnect', connectionId: connection.id })}
                >
                  <WifiOff size={14} />
                  {t('断开', 'Disconnect')}
                </button>
              ) : (
                <button
                  className="button primary"
                  onClick={() => dispatch({ type: 'connect', connectionId: connection.id })}
                >
                  <Wifi size={14} />
                  {t('连接', 'Connect')}
                </button>
              )}
            </div>
            <button
              className="connection-preview"
              onClick={() => {
                const c = state.conversations.find((c) => c.connectionId === connection.id);
                if (c) selectConversation(c.id);
              }}
            >
              {t('查看会话', 'View conversations')}
              <ArrowRight size={14} />
            </button>
          </article>
        ))}
      </div>
      <div className="connection-explainer">
        <div className="explainer-icon">
          <Cable size={24} />
        </div>
        <div>
          <h3>
            {t('会话各自独立，基础资料一起用。', 'Separate conversations. A shared foundation.')}
          </h3>
          <p>
            {t(
              '共享工作区和公共记忆，让不同入口的助手有相同的基础。个人偏好和聊天上下文各自保存。应用关闭后，连接和执行一起停止。',
              'Your workspace and shared memory provide common ground. Personal preferences and conversation context stay separate. Closing the app stops connections and execution.',
            )}
          </p>
        </div>
      </div>
      {editing && (
        <Modal
          title={t('配置演示连接', 'Configure demo connection')}
          close={() => setEditing(null)}
        >
          <div className="modal-body">
            <div className="connection-modal-brand">
              <PlatformIcon platform={editing.platform} size={26} />
              <strong>{editing.name}</strong>
            </div>
            <label className="form-field">
              {t('示例账号名称', 'Sample account name')}
              <input value={account} onChange={(e) => setAccount(e.target.value)} autoFocus />
            </label>
            <label className="form-field">
              {t('演示连接结果', 'Demo connection result')}
              <select
                value={simulateFailure ? 'error' : 'success'}
                onChange={(e) => setSimulateFailure(e.target.value === 'error')}
              >
                <option value="success">{t('连接成功', 'Successful connection')}</option>
                <option value="error">{t('连接异常', 'Connection error')}</option>
              </select>
            </label>
            <p className="field-help">
              {t(
                '这里只保存演示配置，不会连接真实账号，也不需要密钥。',
                'This saves a demo configuration. No real account is connected and no key is needed.',
              )}
            </p>
            <div className="modal-actions">
              <button className="button secondary" onClick={() => setEditing(null)}>
                {t('取消', 'Cancel')}
              </button>
              <button
                className="button primary"
                disabled={!account.trim()}
                onClick={() => {
                  dispatch({
                    type: 'connect',
                    connectionId: editing.id,
                    account: account.trim(),
                    simulateFailure,
                  });
                  setEditing(null);
                }}
              >
                {t('保存并模拟连接', 'Save and simulate connection')}
              </button>
            </div>
          </div>
        </Modal>
      )}
    </div>
  );
}

export function MemoryPage() {
  const { state, dispatch, t, toast } = useApp();
  const [query, setQuery] = useState('');
  const [scope, setScope] = useState('all');
  const [source, setSource] = useState('all');
  const [editing, setEditing] = useState<Note | null>(null);
  const [deleting, setDeleting] = useState(false);
  const notes = state.notes.filter(
    (n) =>
      (scope === 'all' || n.scope === scope) &&
      (source === 'all' || n.source === source) &&
      `${n.title} ${n.body}`.toLocaleLowerCase().includes(query.toLocaleLowerCase()),
  );
  return (
    <div className="page-scroll">
      <PageHeading
        eyebrow={t('一点一滴，慢慢了解', 'THE THINGS WORTH KEEPING')}
        title={t('记住的，是对你有用的。', 'A little understanding goes a long way.')}
        detail={t(
          '常用的资料、做事的习惯，还有不必重复说明的小事。',
          'Useful context, familiar preferences, and the little things you shouldn’t have to repeat.',
        )}
      >
        <button
          className={`switch-label ${state.settings.memory ? 'on' : ''}`}
          role="switch"
          aria-checked={state.settings.memory}
          onClick={() => dispatch({ type: 'settings', value: { memory: !state.settings.memory } })}
        >
          <span className="switch">
            <i />
          </span>
          {t('自动记忆', 'Auto memory')}
        </button>
      </PageHeading>
      {!state.settings.memory && (
        <div className="gentle-notice">
          {t(
            '自动记忆已关闭，已有记录会继续保留。',
            'Auto memory is off. Your existing notes are kept.',
          )}
        </div>
      )}
      <div className="filter-toolbar">
        <div className="segmented">
          {[
            ['all', t('全部记忆', 'All memories')],
            ['shared', t('共享基础', 'Shared')],
            ['personal', t('个人记忆', 'Personal')],
          ].map(([value, label]) => (
            <button
              className={scope === value ? 'active' : ''}
              key={value}
              onClick={() => setScope(value)}
            >
              {label}
            </button>
          ))}
        </div>
        <select
          aria-label={t('记忆来源', 'Memory source')}
          value={source}
          onChange={(e) => setSource(e.target.value)}
        >
          <option value="all">{t('所有来源', 'All sources')}</option>
          {[...new Set(state.notes.map((n) => n.source))].map((s) => (
            <option key={s}>{s}</option>
          ))}
        </select>
      </div>
      <label className="search-field">
        <Search size={16} />
        <input
          aria-label={t('搜索记忆', 'Search memories')}
          placeholder={t('找一条记忆…', 'Find a memory…')}
          value={query}
          onChange={(e) => setQuery(e.target.value)}
        />
      </label>
      <div className="memory-grid">
        {notes.map((note) => (
          <button
            className={`memory-card ${note.scope}`}
            key={note.id}
            onClick={() => {
              setEditing({ ...note });
              setDeleting(false);
            }}
          >
            <div className="memory-card-top">
              <span>
                {note.scope === 'shared' ? <Globe2 size={15} /> : <Leaf size={15} />}
                {note.scope === 'shared'
                  ? t('共享基础', 'Shared foundation')
                  : t('个人记忆', 'Personal memory')}
              </span>
              <Pencil size={14} />
            </div>
            <h3>{note.title}</h3>
            <p>{note.body}</p>
            <div className="memory-card-bottom">
              <span>{note.source}</span>
              <span>
                {new Intl.DateTimeFormat(state.settings.language === 'zh' ? 'zh-CN' : 'en', {
                  month: 'short',
                  day: 'numeric',
                }).format(note.updatedAt)}
              </span>
            </div>
          </button>
        ))}
      </div>
      {!notes.length && (
        <Empty
          icon={<Leaf size={30} />}
          title={t('还没有这样的记忆', 'Nothing here just yet')}
          detail={t(
            '换一个搜索词，或在对话里告诉助手值得记住的事。',
            'Try another search, or tell your assistant something worth remembering.',
          )}
        />
      )}
      <div className="page-note">
        <ShieldCheck size={14} />
        {t(
          '共享知识与个人偏好分别保存，不会自动合并不同人的记忆。',
          'Shared knowledge and personal preferences stay separate. Different people’s memories are never automatically merged.',
        )}
      </div>
      {editing && (
        <Modal title={t('一条记忆', 'A little memory')} close={() => setEditing(null)}>
          <div className="modal-body">
            <span className="memory-scope-label">
              {editing.scope === 'shared' ? <Globe2 size={14} /> : <LockKeyhole size={14} />}
              {editing.scope === 'shared' ? t('共享基础', 'Shared foundation') : editing.source}
            </span>
            <label className="form-field">
              {t('标题', 'Title')}
              <input
                value={editing.title}
                onChange={(e) => setEditing({ ...editing, title: e.target.value })}
                autoFocus
              />
            </label>
            <label className="form-field">
              {t('内容', 'Content')}
              <textarea
                rows={5}
                value={editing.body}
                onChange={(e) => setEditing({ ...editing, body: e.target.value })}
              />
            </label>
            <p className="field-help">
              {t('来源', 'Source')}：{editing.source}
            </p>
            {deleting && (
              <div className="error-note">
                {t(
                  '只删除这条记忆，聊天记录和其他资料会保留。再次点击确认删除。',
                  'Only this memory will be deleted. Conversations and other material are kept. Click again to confirm.',
                )}
              </div>
            )}
            <div className="modal-actions">
              <button
                className="text-button danger"
                onClick={() => {
                  if (!deleting) {
                    setDeleting(true);
                    return;
                  }
                  dispatch({ type: 'note-delete', id: editing.id });
                  setEditing(null);
                  toast(t('记忆已删除', 'Memory deleted'));
                }}
              >
                <Trash2 size={14} />
                {deleting ? t('确认删除', 'Confirm deletion') : t('删除', 'Delete')}
              </button>
              <button
                className="button primary"
                disabled={!editing.title.trim() || !editing.body.trim()}
                onClick={() => {
                  dispatch({ type: 'note-save', note: { ...editing, updatedAt: Date.now() } });
                  setEditing(null);
                  toast(t('记忆已更新', 'Memory updated'));
                }}
              >
                {t('保存记忆', 'Save memory')}
              </button>
            </div>
          </div>
        </Modal>
      )}
    </div>
  );
}

export function SettingsPage({
  reset,
  exportSnapshot,
}: {
  reset: () => void;
  exportSnapshot: () => void;
}) {
  const { state, dispatch, t, toast } = useApp();
  const [confirmReset, setConfirmReset] = useState(false);
  const update = (value: Partial<typeof state.settings>) => dispatch({ type: 'settings', value });
  return (
    <div className="page-scroll settings-page">
      <PageHeading
        eyebrow={t('按你的习惯来', 'MAKE YOURSELF AT HOME')}
        title={t('用着顺手，才是好助手。', 'A little more like you.')}
        detail={t(
          '调整外观和偏好，让 Orangutan 融入你的日常。',
          'Settle on the look and habits that fit your everyday.',
        )}
      />
      <section className="settings-section">
        <h2>
          <SlidersHorizontal size={17} />
          {t('外观与语言', 'Appearance & language')}
        </h2>
        <div className="setting-row">
          <div>
            <strong>{t('主题', 'Theme')}</strong>
            <p>{t('浅色暖一点，深色安静一点。', 'A warmer light. A quieter dark.')}</p>
          </div>
          <div className="theme-options">
            {(
              [
                { id: 'light', icon: Sun, label: t('浅色', 'Light') },
                { id: 'dark', icon: Moon, label: t('深色', 'Dark') },
                { id: 'system', icon: Monitor, label: t('跟随系统', 'System') },
              ] as const
            ).map(({ id, icon: Icon, label }) => (
              <button
                key={id}
                data-testid={`theme-${id}`}
                className={state.settings.theme === id ? 'active' : ''}
                aria-pressed={state.settings.theme === id}
                onClick={() => update({ theme: id })}
              >
                <Icon size={17} />
                {label}
                {state.settings.theme === id && <Check size={12} />}
              </button>
            ))}
          </div>
        </div>
        <div className="setting-row">
          <div>
            <strong>{t('界面语言', 'Interface language')}</strong>
            <p>
              {t('只切换界面，不改写聊天内容。', 'Changes the interface, not your conversations.')}
            </p>
          </div>
          <select
            aria-label={t('界面语言', 'Interface language')}
            data-testid="language-select"
            value={state.settings.language}
            onChange={(e) => update({ language: e.target.value as 'zh' | 'en' })}
          >
            <option value="zh">简体中文</option>
            <option value="en">English</option>
          </select>
        </div>
      </section>
      <section className="settings-section">
        <h2>
          <Settings2 size={17} />
          {t('助手偏好', 'Assistant preferences')}
        </h2>
        <div className="setting-row">
          <div>
            <strong>{t('示例模型', 'Sample model')}</strong>
            <p>
              {t(
                '用于展示模型选择，未连接真实服务。',
                'A model picker demonstration. No live service is connected.',
              )}
            </p>
          </div>
          <select
            aria-label={t('示例模型', 'Sample model')}
            value={state.settings.model}
            onChange={(e) => update({ model: e.target.value as typeof state.settings.model })}
          >
            <option>Claude Sonnet</option>
            <option>GPT-5</option>
          </select>
        </div>
        <div className="setting-row">
          <div>
            <strong>{t('应用内通知', 'In-app notifications')}</strong>
            <p>
              {t(
                '任务有了结果，或有事情需要你关注时提醒。',
                'A heads-up when something is ready or needs attention.',
              )}
            </p>
          </div>
          <button
            className={`toggle ${state.settings.notifications ? 'on' : ''}`}
            role="switch"
            aria-label={t('应用内通知', 'In-app notifications')}
            aria-checked={state.settings.notifications}
            onClick={() => update({ notifications: !state.settings.notifications })}
          >
            <i />
          </button>
        </div>
        <div className="setting-row">
          <div>
            <strong>{t('自动记忆', 'Auto memory')}</strong>
            <p>
              {t(
                '积累有用的信息，关闭后保留已有记录。',
                'Keep useful context. Turning this off preserves existing notes.',
              )}
            </p>
          </div>
          <button
            className={`toggle ${state.settings.memory ? 'on' : ''}`}
            role="switch"
            aria-label={t('自动记忆', 'Auto memory')}
            aria-checked={state.settings.memory}
            onClick={() => update({ memory: !state.settings.memory })}
          >
            <i />
          </button>
        </div>
      </section>
      <section className="settings-section">
        <h2>
          <ShieldCheck size={17} />
          {t('桌面对话权限', 'Desktop conversation permissions')}
        </h2>
        <div className="setting-row">
          <div>
            <strong>{t('文件操作', 'File actions')}</strong>
            <p>
              {t(
                '仅影响桌面自己的对话，不改变其他渠道。',
                'Applies to desktop conversations only. Channel rules stay separate.',
              )}
            </p>
          </div>
          <select
            aria-label={t('文件操作权限', 'File action permission')}
            value={state.settings.permission}
            onChange={(e) =>
              update({ permission: e.target.value as typeof state.settings.permission })
            }
          >
            <option value="ask">{t('操作前询问', 'Ask before acting')}</option>
            <option value="allow">{t('允许演示操作', 'Allow demo actions')}</option>
            <option value="deny">{t('拒绝文件操作', 'Deny file actions')}</option>
          </select>
        </div>
      </section>
      <section className="settings-section">
        <h2>
          <FolderOpen size={17} />
          {t('工作区与演示数据', 'Workspace & demo data')}
        </h2>
        <div className="setting-row">
          <div>
            <strong>{t('共享工作区', 'Shared workspace')}</strong>
            <p>
              {t(
                '所有示例资料都在演示空间里，不读取本机文件。',
                'Sample materials live in the demo. Local files are not accessed.',
              )}
            </p>
          </div>
          <code>workspace/</code>
        </div>
        <div className="setting-row">
          <div>
            <strong>{t('本地数据', 'Local data')}</strong>
            <p>
              {t(
                '存储在此浏览器中，关闭应用后执行停止。',
                'Saved in this browser. Execution stops when the app closes.',
              )}
            </p>
          </div>
          <div className="setting-actions">
            <button className="button secondary" onClick={exportSnapshot}>
              <ArrowDownToLine size={14} />
              {t('导出', 'Export')}
            </button>
            <button className="text-button" onClick={() => setConfirmReset(true)}>
              {t('重新开始演示', 'Restart demo')}
            </button>
          </div>
        </div>
      </section>
      <div className="settings-signature">
        <img src="/orangutan.svg" alt="" />
        <span>
          Orangutan <span>·</span> {t('留点空间，给生活。', 'A little more headspace.')}
        </span>
      </div>
      {confirmReset && (
        <Modal
          title={t('重新开始演示？', 'Start the demo again?')}
          close={() => setConfirmReset(false)}
        >
          <div className="modal-body">
            <p>
              {t(
                '当前快照会先保留为浏览器中的备份，再加载初始示例。也可以先导出一份文件。',
                'Your current snapshot will be backed up in this browser before loading the initial examples. You can also export a copy first.',
              )}
            </p>
            <div className="modal-actions">
              <button className="button secondary" onClick={exportSnapshot}>
                {t('导出备份', 'Export backup')}
              </button>
              <button
                className="button primary"
                onClick={() => {
                  reset();
                  setConfirmReset(false);
                  toast(t('已请求重置演示', 'Demo reset requested'));
                }}
              >
                {t('确认重新开始', 'Start again')}
              </button>
            </div>
          </div>
        </Modal>
      )}
    </div>
  );
}
