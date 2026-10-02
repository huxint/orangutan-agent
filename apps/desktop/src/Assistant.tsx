import { Fragment, useEffect, useRef, useState, type RefObject } from 'react';
import {
  ArrowRight,
  ArrowUp,
  Check,
  FileText,
  LockKeyhole,
  Monitor,
  Paperclip,
  Search,
  Square,
  Users,
} from 'lucide-react';
import { useApp } from './context.ts';
import { active } from './model.ts';
import { Approval, Logo, Markdown, Source, StatusBadge, TaskCard } from './components.tsx';

function Composer({ inputRef }: { inputRef: RefObject<HTMLTextAreaElement | null> }) {
  const { state, dispatch, t, toast } = useApp();
  const conversationId = state.selectedId;
  const draft = state.drafts[conversationId] ?? '';
  const running = state.tasks.find(
    (task) => task.conversationId === conversationId && active(task.status),
  );
  const send = () => {
    if (draft.trim() && !running)
      dispatch({
        type: 'send',
        conversationId,
        text: draft,
        id: crypto.randomUUID(),
        now: Date.now(),
      });
  };
  return (
    <div className="composer-wrap">
      <div className="composer" data-testid="composer">
        <textarea
          ref={inputRef}
          aria-label={t('给助手发消息', 'Message your assistant')}
          placeholder={t('描述任务，或输入问题…', 'Describe a task or ask a question…')}
          value={draft}
          rows={3}
          onChange={(e) => dispatch({ type: 'draft', conversationId, text: e.target.value })}
          onKeyDown={(e) => {
            if (e.key === 'Enter' && !e.shiftKey && !e.nativeEvent.isComposing) {
              e.preventDefault();
              send();
            }
          }}
        />
        <div className="composer-tools">
          <button
            className="icon-button"
            aria-label={t('添加示例资料', 'Attach sample notes')}
            onClick={() => {
              dispatch({
                type: 'draft',
                conversationId,
                text:
                  draft +
                  (draft ? '\n' : '') +
                  t(
                    '请参考示例文件 meeting-notes.md，整理会议笔记。',
                    'Organize the sample meeting-notes.md file.',
                  ),
              });
              inputRef.current?.focus();
              toast(t('已添加示例资料', 'Sample notes attached'));
            }}
          >
            <Paperclip size={16} />
          </button>
          <label className="composer-model">
            <select
              aria-label={t('选择示例模型', 'Choose sample model')}
              value={state.settings.model}
              onChange={(e) =>
                dispatch({
                  type: 'settings',
                  value: { model: e.target.value as typeof state.settings.model },
                })
              }
            >
              <option>Claude Sonnet</option>
              <option>GPT-5</option>
            </select>
          </label>
          <span className="composer-spacer" />
          <span className="composer-scope">
            <Monitor size={12} />
            {t('本地会话', 'Local session')}
          </span>
          {running ? (
            <button
              className="send-button stop"
              aria-label={t('停止本地任务', 'Stop local task')}
              disabled={running.status === 'cancelling'}
              onClick={() => dispatch({ type: 'stop', taskId: running.id, now: Date.now() })}
            >
              <Square size={14} fill="currentColor" />
            </button>
          ) : (
            <button
              className="send-button"
              aria-label={t('发送消息', 'Send message')}
              disabled={!draft.trim()}
              onClick={send}
            >
              <ArrowUp size={17} />
            </button>
          )}
        </div>
      </div>
      <div className="composer-caption">
        <span>
          {t(
            '演示模式 · 模型与执行结果为模拟数据',
            'Demo · Model replies and execution are simulated',
          )}
        </span>
        <span>
          {t('Enter 发送 · Shift + Enter 换行', 'Enter to send · Shift + Enter for a new line')}
        </span>
      </div>
    </div>
  );
}

export function Assistant() {
  const { state, dispatch, t, openTask, selectConversation, navigate } = useApp();
  const conversation = state.conversations.find((c) => c.id === state.selectedId)!;
  const local = conversation.origin === 'desktop';
  const messages = state.messages.filter((m) => m.conversationId === conversation.id);
  const tasks = state.tasks.filter((task) => task.conversationId === conversation.id);
  const running = tasks.find((task) => active(task.status));
  const scroll = useRef<HTMLDivElement>(null);
  const textarea = useRef<HTMLTextAreaElement>(null);
  const pinned = useRef(true);
  const jumping = useRef(false);
  const [atBottom, setAtBottom] = useState(true);
  const [source, setSource] = useState('all');
  const lastText = messages.at(-1)?.text;
  useEffect(() => {
    pinned.current = true;
    jumping.current = false;
    setAtBottom(true);
    const element = scroll.current;
    const content = element?.firstElementChild;
    if (!element || !content) return;
    element.scrollTop = element.scrollHeight;
    const observer = new ResizeObserver(() => {
      if (pinned.current && !jumping.current) element.scrollTop = element.scrollHeight;
    });
    observer.observe(content);
    return () => observer.disconnect();
  }, [conversation.id, messages.length === 0]);
  useEffect(() => {
    if (scroll.current && pinned.current && !jumping.current)
      scroll.current.scrollTop = scroll.current.scrollHeight;
  }, [lastText, running?.status]);
  const cancelJump = () => {
    if (!jumping.current) return;
    jumping.current = false;
    pinned.current = false;
    setAtBottom(false);
  };
  const suggest = (text: string) => {
    dispatch({ type: 'draft', conversationId: conversation.id, text });
    textarea.current?.focus();
  };
  const prompts = [
    {
      icon: FileText,
      title: t('整理资料', 'Organize files'),
      prompt: t(
        '汇总工作区中的会议记录，列出决策、负责人和待办事项。',
        'Summarize the workspace meeting notes, including decisions, owners and action items.',
      ),
    },
    {
      icon: Users,
      title: t('拆解计划', 'Plan a task'),
      prompt: t(
        '组织小队，制定下周的工作计划，对比优先级和时间安排。',
        'Plan next week’s work with a team. Compare priorities and scheduling options.',
      ),
    },
    {
      icon: Search,
      title: t('分析问题', 'Analyze a question'),
      prompt: t(
        '帮我分析这个问题：如何减少日常工作中的重复整理？',
        'Help me analyze ways to reduce repetitive organization in my daily work.',
      ),
    },
  ];
  const recent = [...state.tasks]
    .filter(
      (task) =>
        source === 'all' ||
        state.conversations.find((c) => c.id === task.conversationId)?.origin === source,
    )
    .sort(
      (a, b) => Number(active(b.status)) - Number(active(a.status)) || b.createdAt - a.createdAt,
    )
    .slice(0, 4);
  const empty = messages.length === 0 && local;
  return (
    <div className={'assistant-view' + (empty ? ' is-empty' : '')}>
      {empty ? (
        <div className="start-workspace">
          <section
            className="start-session"
            aria-label={t('开始本地会话', 'Start a local session')}
          >
            <div className="start-heading">
              <h1>{t('今天想处理什么？', 'What can I help with?')}</h1>
            </div>
            <p className="start-description">
              {t(
                '交代一件事，或继续手边的工作。',
                'Start something new, or pick up where you left off.',
              )}
            </p>
            <Composer inputRef={textarea} />
            <div className="request-shortcuts">
              {prompts.map(({ icon: Icon, title, prompt }) => (
                <button key={title} onClick={() => suggest(prompt)}>
                  <Icon size={18} />
                  <span>{title}</span>
                </button>
              ))}
            </div>
          </section>
          <section className="recent-section" aria-label={t('最近任务', 'Recent tasks')}>
            <div className="section-heading">
              <h2>{t('工作动态', 'Your work')}</h2>
              <button className="text-button" onClick={() => navigate('tasks')}>
                {t('查看全部', 'View all')}
                <ArrowRight size={13} />
              </button>
            </div>
            <div className="activity-filters" aria-label={t('筛选任务来源', 'Filter task sources')}>
              {[
                ['all', t('全部', 'All')],
                ['desktop', t('桌面', 'Desktop')],
                ['channel', t('消息渠道', 'Channels')],
              ].map(([value, label]) => (
                <button
                  key={value}
                  aria-pressed={source === value}
                  className={source === value ? 'active' : ''}
                  onClick={() => setSource(value)}
                >
                  {label}
                </button>
              ))}
            </div>
            <div className="activity-list">
              {recent.map((task) => (
                <button
                  className="activity-row"
                  key={task.id}
                  onClick={() => {
                    selectConversation(task.conversationId);
                    openTask(task.id);
                  }}
                >
                  <span className="activity-icon">
                    {task.members.length ? (
                      <Users size={16} />
                    ) : task.status === 'completed' ? (
                      <Check size={16} />
                    ) : (
                      <FileText size={16} />
                    )}
                  </span>
                  <div className="activity-title">
                    <strong>{task.title}</strong>
                    <Source conversationId={task.conversationId} />
                  </div>
                  <div className="activity-meta">
                    <StatusBadge status={task.status} />
                    <time>
                      {new Intl.DateTimeFormat(state.settings.language === 'zh' ? 'zh-CN' : 'en', {
                        hour: '2-digit',
                        minute: '2-digit',
                        hour12: false,
                      }).format(task.createdAt)}
                    </time>
                  </div>
                </button>
              ))}
            </div>
            {!recent.length && (
              <p className="activity-empty">
                {t(
                  '暂无任务。发送请求后，执行记录会显示在这里。',
                  'No tasks yet. Send a request to create an execution record.',
                )}
              </p>
            )}
          </section>
        </div>
      ) : (
        <>
          <div
            className="conversation-scroll"
            ref={scroll}
            tabIndex={-1}
            aria-label={t('会话记录', 'Conversation history')}
            onWheel={cancelJump}
            onTouchStart={cancelJump}
            onPointerDown={cancelJump}
            onKeyDown={(event) => {
              if (
                ['ArrowUp', 'ArrowDown', 'PageUp', 'PageDown', 'Home', 'End', ' '].includes(
                  event.key,
                )
              )
                cancelJump();
            }}
            onScrollEnd={() => {
              if (!jumping.current || !scroll.current) return;
              jumping.current = false;
              scroll.current.scrollTop = scroll.current.scrollHeight;
              pinned.current = true;
              setAtBottom(true);
            }}
            onScroll={() => {
              const element = scroll.current;
              if (!element || jumping.current) return;
              pinned.current = element.scrollHeight - element.scrollTop - element.clientHeight < 64;
              setAtBottom(pinned.current);
            }}
          >
            <div className="thread">
              {!local && (
                <div className="channel-readonly">
                  <LockKeyhole size={14} />
                  <span>
                    {t('渠道会话 · 只读预览', 'Channel conversation · Read-only preview')}
                  </span>
                  <Source conversationId={conversation.id} />
                </div>
              )}
              <div className="thread-date">{t('会话记录', 'Conversation history')}</div>
              {messages.map((message) => (
                <Fragment key={message.id}>
                  <div className={'message ' + message.role}>
                    {message.role === 'assistant' && <Logo small />}
                    <div className="message-body">
                      {message.role === 'assistant' && (
                        <div className="message-author">
                          Orangutan <span>{t('助手', 'Assistant')}</span>
                        </div>
                      )}
                      <Markdown>{message.text}</Markdown>
                      {message.state === 'streaming' && <span className="stream-caret" />}
                      {message.state === 'interrupted' && (
                        <small className="muted">{t('回复已中断', 'Response interrupted')}</small>
                      )}
                    </div>
                  </div>
                  {tasks
                    .filter((task) => message.id === `${task.id}:user`)
                    .map((task) => (
                      <div key={task.id} className="thread-task">
                        <TaskCard task={task} />
                        <Approval task={task} />
                      </div>
                    ))}
                </Fragment>
              ))}
              {tasks
                .filter((task) => !messages.some((message) => message.id === `${task.id}:user`))
                .map((task) => (
                  <div key={task.id} className="thread-task">
                    <TaskCard task={task} />
                    <Approval task={task} />
                  </div>
                ))}
              {running && (
                <div className="thinking">
                  {running.status === 'running' && (
                    <span className="thinking-dots" aria-hidden="true">
                      <i />
                      <i />
                      <i />
                    </span>
                  )}
                  {running.status === 'awaiting_approval'
                    ? t('等待操作确认', 'Awaiting approval')
                    : running.status === 'cancelling'
                      ? t('正在取消执行', 'Cancelling execution')
                      : t('正在执行', 'Executing')}
                </div>
              )}
            </div>
          </div>
          {!atBottom && (
            <button
              className="jump-latest"
              onClick={() => {
                const element = scroll.current;
                if (!element) return;
                const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
                jumping.current = !reduced;
                pinned.current = true;
                setAtBottom(true);
                element.focus({ preventScroll: true });
                element.scrollTo({
                  top: element.scrollHeight,
                  behavior: reduced ? 'instant' : 'smooth',
                });
              }}
            >
              {t('回到最新', 'Jump to latest')}
              <ArrowUp size={14} />
            </button>
          )}
          {local ? (
            <Composer inputRef={textarea} />
          ) : (
            <div className="readonly-footer">
              <LockKeyhole size={14} />
              <div>
                <strong>{t('渠道会话只读', 'Read-only channel conversation')}</strong>
                <span>
                  {t(
                    '发送消息、审批和任务控制请在原渠道进行。',
                    'Send messages, approve actions and control tasks in the original channel.',
                  )}
                </span>
              </div>
            </div>
          )}
        </>
      )}
    </div>
  );
}
