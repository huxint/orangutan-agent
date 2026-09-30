import { useEffect, useState } from 'react';
import {
  ArrowRight,
  Bell,
  Check,
  CheckCheck,
  ChevronDown,
  ChevronRight,
  CircleHelp,
  FolderOpen,
  Leaf,
  Menu,
  MessageCircle,
  Moon,
  Plus,
  Radio,
  Search,
  Settings2,
  Sparkles,
  Sun,
} from 'lucide-react';
import { AppContext, type View } from './context.ts';
import { useAgent } from './useAgent.ts';
import { Assistant } from './Assistant.tsx';
import { ConnectionsPage, MemoryPage, SettingsPage, TasksPage } from './Pages.tsx';
import { TaskPanel } from './TaskPanel.tsx';
import { Empty, Logo, Modal, PlatformIcon, Source, StatusBadge } from './components.tsx';

export default function App() {
  const { state, dispatch, storageError, reset, exportSnapshot } = useAgent();
  const [view, setView] = useState<View>('assistant');
  const [taskId, setTaskId] = useState<string | null>(null);
  const [sidebarOpen, setSidebarOpen] = useState(false);
  const [dialog, setDialog] = useState<'search' | 'notifications' | 'about' | 'reset' | null>(null);
  const [query, setQuery] = useState('');
  const [toast, setToast] = useState('');
  const t = (zh: string, en: string) => (state.settings.language === 'zh' ? zh : en);
  const navigate = (next: View) => {
    setView(next);
    setTaskId(null);
    setSidebarOpen(false);
  };
  const selectConversation = (id: string) => {
    dispatch({ type: 'select', id });
    setView('assistant');
    setTaskId(null);
    setSidebarOpen(false);
  };
  const newConversation = () => {
    dispatch({ type: 'new', id: crypto.randomUUID(), now: Date.now() });
    navigate('assistant');
  };
  const selected = state.conversations.find((c) => c.id === state.selectedId)!;
  const unread = state.notifications.filter((n) => !n.read).length;
  const navItems = [
    { id: 'assistant' as const, icon: MessageCircle, label: t('助手', 'Assistant') },
    { id: 'tasks' as const, icon: CheckCheck, label: t('任务', 'Tasks') },
    { id: 'connections' as const, icon: Radio, label: t('连接', 'Connections') },
    { id: 'memory' as const, icon: Leaf, label: t('记忆', 'Memory') },
    { id: 'settings' as const, icon: Settings2, label: t('设置', 'Settings') },
  ];
  useEffect(() => {
    const media = window.matchMedia('(prefers-color-scheme: dark)');
    const update = () => {
      const dark =
        state.settings.theme === 'dark' || (state.settings.theme === 'system' && media.matches);
      document.documentElement.dataset.theme = dark ? 'dark' : 'light';
      document
        .querySelector('meta[name="theme-color"]')
        ?.setAttribute('content', dark ? '#20231f' : '#f8f7f3');
    };
    update();
    media.addEventListener('change', update);
    return () => media.removeEventListener('change', update);
  }, [state.settings.theme]);
  useEffect(() => {
    document.documentElement.lang = state.settings.language === 'zh' ? 'zh-CN' : 'en';
    document.title = t('Orangutan · 你的本地助手', 'Orangutan · Your local companion');
  }, [state.settings.language]);
  useEffect(() => {
    const handler = (event: KeyboardEvent) => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k') {
        event.preventDefault();
        setDialog((previous) => (previous === 'search' ? null : 'search'));
      }
      if (event.key === 'Escape') setSidebarOpen(false);
    };
    window.addEventListener('keydown', handler);
    return () => window.removeEventListener('keydown', handler);
  }, []);
  useEffect(() => {
    if (!toast) return;
    const timer = setTimeout(() => setToast(''), 3000);
    return () => clearTimeout(timer);
  }, [toast]);
  const matchingConversations = state.conversations.filter((c) =>
    `${c.name} ${c.connectionId}`.toLocaleLowerCase().includes(query.toLocaleLowerCase()),
  );
  const matchingTasks = state.tasks.filter((task) =>
    task.title.toLocaleLowerCase().includes(query.toLocaleLowerCase()),
  );

  return (
    <AppContext.Provider
      value={{
        state,
        dispatch,
        t,
        view,
        navigate,
        selectConversation,
        openTask: setTaskId,
        toast: setToast,
      }}
    >
      <div
        className={`app-shell ${taskId ? 'has-panel' : ''} ${sidebarOpen ? 'sidebar-open' : ''}`}
      >
        {sidebarOpen && (
          <button
            className="sidebar-scrim"
            aria-label={t('关闭导航', 'Close navigation')}
            onClick={() => setSidebarOpen(false)}
          />
        )}
        <aside className="sidebar" aria-label={t('主导航', 'Main navigation')}>
          <button
            className="brand"
            onClick={() => navigate('assistant')}
            aria-label={t('Orangutan 首页', 'Orangutan home')}
          >
            <Logo />
            <span>
              <strong>
                orangutan<span className="brand-period">.</span>
              </strong>
              <small>{t('你的本地助手', 'YOUR LOCAL COMPANION')}</small>
            </span>
          </button>
          <button className="new-conversation" onClick={newConversation}>
            <Plus size={17} />
            <span>{t('新的对话', 'New conversation')}</span>
            <Sparkles className="new-spark" size={16} />
          </button>
          <nav>
            {navItems.map(({ id, icon: Icon, label }) => (
              <button
                key={id}
                data-testid={`nav-${id}`}
                className={`nav-item ${view === id ? 'active' : ''}`}
                aria-current={view === id ? 'page' : undefined}
                onClick={() => navigate(id)}
              >
                <Icon size={18} strokeWidth={1.6} />
                <span>{label}</span>
                {id === 'tasks' && (
                  <span className="nav-count">
                    {
                      state.tasks.filter((task) =>
                        ['queued', 'running', 'awaiting_approval'].includes(task.status),
                      ).length
                    }
                  </span>
                )}
              </button>
            ))}
          </nav>
          <div className="sidebar-history">
            <div className="sidebar-section-label">
              {t('对话空间', 'CONVERSATIONS')}
              <span>{state.conversations.length}</span>
            </div>
            <div className="conversation-group-label">
              <span className="source-dot desktop" />
              {t('在这台电脑上', 'On this computer')}
            </div>
            {state.conversations
              .filter((c) => c.origin === 'desktop')
              .map((c) => (
                <button
                  className={`conversation-link ${view === 'assistant' && state.selectedId === c.id ? 'selected' : ''}`}
                  key={c.id}
                  onClick={() => selectConversation(c.id)}
                >
                  <MessageCircle size={13} />
                  <span>{c.name}</span>
                </button>
              ))}
            <div className="conversation-group-label channels-label">
              {t('来自消息渠道', 'FROM YOUR CHANNELS')}
            </div>
            {state.conversations
              .filter((c) => c.origin === 'channel')
              .map((c) => {
                const connection = state.connections.find((v) => v.id === c.connectionId)!;
                return (
                  <button
                    className={`conversation-link channel-link ${view === 'assistant' && state.selectedId === c.id ? 'selected' : ''}`}
                    key={c.id}
                    data-testid={`conversation-${connection.id}`}
                    onClick={() => selectConversation(c.id)}
                  >
                    <PlatformIcon platform={connection.platform} size={12} />
                    <span>
                      {c.name}
                      <small>{connection.name}</small>
                    </span>
                    {c.unread > 0 && <span className="unread-dot" />}
                  </button>
                );
              })}
          </div>
          <div className="sidebar-bottom">
            <button className="workspace-switch" onClick={() => navigate('settings')}>
              <span className="workspace-avatar">
                <FolderOpen size={17} />
              </span>
              <span>
                <strong>{t('本地工作区', 'Local workspace')}</strong>
                <small>
                  <span />
                  {t('此设备 · 演示模式', 'This device · Demo')}
                </small>
              </span>
              <ChevronDown size={13} />
            </button>
            <div className="sidebar-footer">
              <span>MADE FOR YOUR EVERYDAY</span>
              <button
                className="icon-button"
                aria-label={t('关于演示', 'About this demo')}
                onClick={() => setDialog('about')}
              >
                <CircleHelp size={14} />
              </button>
            </div>
          </div>
        </aside>
        <div className="workspace">
          <header className="topbar">
            <button
              className="icon-button menu-button"
              aria-label={t('打开导航', 'Open navigation')}
              aria-expanded={sidebarOpen}
              onClick={() => setSidebarOpen(!sidebarOpen)}
            >
              <Menu size={19} />
            </button>
            <div className="breadcrumb">
              <span>{navItems.find((n) => n.id === view)?.label}</span>
              {view === 'assistant' && (
                <>
                  <ChevronRight size={12} />
                  <strong>{selected.name}</strong>
                </>
              )}
            </div>
            <div className="topbar-actions">
              <button
                className="topbar-search"
                onClick={() => {
                  setQuery('');
                  setDialog('search');
                }}
                aria-label={t('搜索全部内容', 'Search everything')}
              >
                <Search size={15} />
                <span>{t('搜索', 'Search')}</span>
                <kbd>Ctrl K</kbd>
              </button>
              <button className="demo-badge" onClick={() => setDialog('about')}>
                <span />
                {t('演示模式', 'Demo mode')}
              </button>
              <span className="topbar-divider" />
              <button
                className="icon-button theme-shortcut"
                aria-label={t('切换明暗主题', 'Toggle color theme')}
                onClick={() =>
                  dispatch({
                    type: 'settings',
                    value: {
                      theme: document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark',
                    },
                  })
                }
              >
                {state.settings.theme === 'dark' ? <Sun size={17} /> : <Moon size={17} />}
              </button>
              <button
                className="icon-button notification-button"
                aria-label={t('通知', 'Notifications')}
                onClick={() => setDialog('notifications')}
              >
                <Bell size={18} />
                {unread > 0 && <span className="notification-count">{unread}</span>}
              </button>
            </div>
          </header>
          {storageError && (
            <div className="storage-banner" role="alert">
              <span>
                {t(
                  '本地快照暂时无法读取或保存。已有数据不会被自动清除。',
                  'The local snapshot could not be read or saved. Existing data will not be automatically cleared.',
                )}
              </span>
              <button onClick={exportSnapshot}>{t('导出', 'Export')}</button>
              <button onClick={() => setDialog('reset')}>{t('恢复演示', 'Recover demo')}</button>
            </div>
          )}
          <div className="workspace-body">
            <main className="main-content" id="main-content" tabIndex={-1}>
              {view === 'assistant' ? (
                <Assistant />
              ) : view === 'tasks' ? (
                <TasksPage />
              ) : view === 'connections' ? (
                <ConnectionsPage />
              ) : view === 'memory' ? (
                <MemoryPage />
              ) : (
                <SettingsPage reset={reset} exportSnapshot={exportSnapshot} />
              )}
            </main>
            {taskId && <TaskPanel key={taskId} id={taskId} close={() => setTaskId(null)} />}
          </div>
        </div>
        {toast && (
          <div className="toast" role="status">
            <Check size={15} />
            {toast}
          </div>
        )}
        {dialog === 'search' && (
          <Modal
            title={t('找一段对话，或一件事', 'Find a conversation or a task')}
            close={() => setDialog(null)}
            wide
          >
            <div className="modal-body">
              <label className="search-field modal-search">
                <Search size={18} />
                <input
                  autoFocus
                  aria-label={t('搜索关键词', 'Search query')}
                  value={query}
                  placeholder={t('输入名称、渠道或任务…', 'Search names, channels or tasks…')}
                  onChange={(e) => setQuery(e.target.value)}
                />
                <kbd>Esc</kbd>
              </label>
              <div className="search-results">
                <h3>{t('对话', 'Conversations')}</h3>
                {matchingConversations.map((c) => (
                  <button
                    key={c.id}
                    onClick={() => {
                      selectConversation(c.id);
                      setDialog(null);
                    }}
                  >
                    <MessageCircle size={16} />
                    <span>
                      {c.name}
                      <small>{c.connectionId}</small>
                    </span>
                    <ArrowRight size={14} />
                  </button>
                ))}
                <h3>{t('任务', 'Tasks')}</h3>
                {matchingTasks.map((task) => (
                  <button
                    key={task.id}
                    onClick={() => {
                      selectConversation(task.conversationId);
                      setTaskId(task.id);
                      setDialog(null);
                    }}
                  >
                    <Sparkles size={16} />
                    <span>{task.title}</span>
                    <StatusBadge status={task.status} />
                  </button>
                ))}
                {!matchingConversations.length && !matchingTasks.length && (
                  <Empty
                    title={t('没有找到', 'No matches yet')}
                    detail={t('换个关键词再试试。', 'Try another word.')}
                  />
                )}
              </div>
            </div>
          </Modal>
        )}
        {dialog === 'notifications' && (
          <Modal title={t('新的动静', 'A few updates')} close={() => setDialog(null)}>
            <div className="notification-list">
              {state.notifications.map((n) => (
                <div className={`notification-item ${n.read ? 'read' : ''}`} key={n.id}>
                  <span className={`activity-icon ${n.kind === 'approval' ? 'orange' : 'green'}`}>
                    {n.kind === 'completed' ? <Check size={17} /> : <Bell size={17} />}
                  </span>
                  <button
                    className="notification-content"
                    onClick={() => {
                      dispatch({ type: 'read-notification', id: n.id });
                      if (n.conversationId) selectConversation(n.conversationId);
                      else navigate('connections');
                      if (n.taskId) setTaskId(n.taskId);
                      setDialog(null);
                    }}
                  >
                    <strong>{n.title}</strong>
                    <p>{n.detail}</p>
                    {n.conversationId && <Source conversationId={n.conversationId} />}
                  </button>
                  {!n.read && (
                    <button
                      className="icon-button"
                      aria-label={t('标记已读', 'Mark as read')}
                      onClick={() => dispatch({ type: 'read-notification', id: n.id })}
                    >
                      <CheckCheck size={15} />
                    </button>
                  )}
                </div>
              ))}
              {!state.notifications.length && (
                <Empty
                  title={t('暂时没有新消息', 'All caught up')}
                  detail={t(
                    '有新的进展时，会出现在这里。',
                    'New updates will find their way here.',
                  )}
                />
              )}
            </div>
          </Modal>
        )}
        {dialog === 'about' && (
          <Modal title={t('认识一下 Orangutan', 'Meet Orangutan')} close={() => setDialog(null)}>
            <div className="modal-body about-demo">
              <Logo />
              <h3>{t('一个能搭把手的本地助手。', 'A local companion for the everyday.')}</h3>
              <p>
                {t(
                  '这是一个可交互的桌面界面原型。回复、小队工作和渠道连接均使用演示数据；不会发送真实消息、调用模型或修改本机文件。',
                  'This is an interactive desktop interface prototype. Replies, teamwork and channel connections use demo data. It sends no real messages, calls no model and changes no local files.',
                )}
              </p>
              <p>
                {t(
                  '历史保存在当前浏览器中。关闭或刷新页面后，未完成工作会标为中断。',
                  'History is saved in this browser. Unfinished work is marked interrupted after closing or refreshing the page.',
                )}
              </p>
              <button className="button primary" onClick={() => setDialog(null)}>
                {t('开始逛逛', 'Make yourself at home')}
                <ArrowRight size={15} />
              </button>
            </div>
          </Modal>
        )}
        {dialog === 'reset' && (
          <Modal title={t('恢复演示数据', 'Recover demo data')} close={() => setDialog(null)}>
            <div className="modal-body">
              <p>
                {t(
                  '已有快照会先保留为本地备份，再加载初始示例。存储不可用时不会强行覆盖。',
                  'The existing snapshot is backed up before initial examples are loaded. Unavailable storage will not be overwritten.',
                )}
              </p>
              <div className="modal-actions">
                <button className="button secondary" onClick={exportSnapshot}>
                  {t('导出快照', 'Export snapshot')}
                </button>
                <button
                  className="button primary"
                  onClick={() => {
                    reset();
                    setDialog(null);
                    setTaskId(null);
                  }}
                >
                  {t('确认恢复', 'Confirm recovery')}
                </button>
              </div>
            </div>
          </Modal>
        )}
      </div>
    </AppContext.Provider>
  );
}
