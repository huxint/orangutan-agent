import { useEffect, useRef } from 'react';
import {
  ArrowRight,
  ArrowUp,
  Check,
  FileText,
  Leaf,
  LockKeyhole,
  Paperclip,
  Sparkles,
  Square,
  Users,
  WandSparkles,
} from 'lucide-react';
import { useApp } from './context.ts';
import { active } from './model.ts';
import {
  Approval,
  Logo,
  Markdown,
  PlantIllustration,
  Source,
  StatusBadge,
  TaskCard,
} from './components.tsx';

export function Assistant() {
  const { state, dispatch, t, openTask, selectConversation, toast } = useApp();
  const conversation = state.conversations.find((c) => c.id === state.selectedId)!;
  const local = conversation.origin === 'desktop';
  const messages = state.messages.filter((m) => m.conversationId === conversation.id);
  const tasks = state.tasks.filter((task) => task.conversationId === conversation.id);
  const running = tasks.find((task) => active(task.status));
  const draft = state.drafts[conversation.id] ?? '';
  const scroll = useRef<HTMLDivElement>(null);
  const textarea = useRef<HTMLTextAreaElement>(null);
  const lastText = messages.at(-1)?.text;
  useEffect(() => {
    if (scroll.current && messages.length) scroll.current.scrollTop = scroll.current.scrollHeight;
  }, [conversation.id, lastText, running?.status]);
  const send = () => {
    if (draft.trim() && !running)
      dispatch({
        type: 'send',
        conversationId: conversation.id,
        text: draft,
        id: crypto.randomUUID(),
        now: Date.now(),
      });
  };
  const suggest = (text: string) => {
    dispatch({ type: 'draft', conversationId: conversation.id, text });
    textarea.current?.focus();
  };
  const prompts = [
    {
      icon: FileText,
      title: t('把资料整理清楚', 'Bring a little order'),
      subtitle: t('从一份笔记开始', 'Start with a few notes'),
      prompt: t(
        '帮我整理一下工作区的会议笔记，先给我看看方案。',
        'Help me organize the meeting notes in my workspace. Show me a plan first.',
      ),
    },
    {
      icon: Users,
      title: t('一起想个好计划', 'Make room for a plan'),
      subtitle: t('让小队搭把手', 'A few minds are better'),
      prompt: t(
        '组织一个小队，帮我计划一个轻松的周末，留些时间散步和读书。',
        'Put together a team to plan a relaxed weekend, with time for a walk and a book.',
      ),
    },
    {
      icon: WandSparkles,
      title: t('聊聊新的想法', 'Follow a small idea'),
      subtitle: t('想到什么，都可以', 'Whatever is on your mind'),
      prompt: t(
        '我有些零散的想法，想和你一起理一理。',
        'I have a few scattered ideas. Can you help me think them through?',
      ),
    },
  ];
  return (
    <div className="assistant-view">
      <div className="conversation-scroll" ref={scroll}>
        {messages.length === 0 && local ? (
          <div className="welcome">
            <div className="welcome-hero">
              <div>
                <div className="eyebrow">
                  <span />
                  {t('有我在，慢慢来', 'A LITTLE HELP. A LITTLE HEADSPACE.')}
                </div>
                <h1>
                  {t('把手头的事，', 'A little less to do.')}
                  <br />
                  <em>{t('交给我吧。', 'A little more room.')}</em>
                </h1>
                <p>
                  {t(
                    '一个想法，一份资料，或一个还没开始的计划。',
                    'An idea, a document, or a plan you haven’t quite started.',
                  )}
                  <br />
                  {t('我们一起，让事情变得简单一点。', 'Let’s make a little progress, together.')}
                </p>
              </div>
              <PlantIllustration />
            </div>
            <div className="prompt-grid">
              {prompts.map(({ icon: Icon, title, subtitle, prompt }) => (
                <button className="prompt-card" key={title} onClick={() => suggest(prompt)}>
                  <Icon size={19} />
                  <strong>{title}</strong>
                  <span>
                    {subtitle}
                    <ArrowUp size={13} />
                  </span>
                </button>
              ))}
            </div>
            <div className="section-heading">
              <h2>{t('最近的动静', 'Around your workspace')}</h2>
              <span>{t('不同入口，同一个工作空间', 'A few channels. One shared space.')}</span>
            </div>
            <div className="activity-list">
              {state.tasks
                .filter((task) => ['qq-files', 'weekend-plan', 'reading-plan'].includes(task.id))
                .map((task) => (
                  <button
                    className="activity-row"
                    key={task.id}
                    onClick={() => {
                      selectConversation(task.conversationId);
                      openTask(task.id);
                    }}
                  >
                    <span
                      className={`activity-icon ${task.status === 'completed' ? 'green' : 'orange'}`}
                    >
                      {task.members.length ? (
                        <Users size={18} />
                      ) : task.status === 'completed' ? (
                        <Check size={18} />
                      ) : (
                        <FileText size={18} />
                      )}
                    </span>
                    <div>
                      <strong>{task.title}</strong>
                      <Source conversationId={task.conversationId} />
                    </div>
                    <StatusBadge status={task.status} />
                    <ArrowRight size={16} className="row-arrow" />
                  </button>
                ))}
            </div>
            <div className="welcome-footnote">
              <Leaf className="tiny-leaf" size={14} />
              {t(
                '记住有用的事，把时间留给你。',
                'Keeping the useful things. Leaving the time to you.',
              )}
            </div>
          </div>
        ) : (
          <div className="thread">
            {!local && (
              <div className="channel-readonly">
                <LockKeyhole size={14} />
                <span>{t('渠道会话 · 只读预览', 'Channel conversation · Read-only preview')}</span>
                <Source conversationId={conversation.id} />
              </div>
            )}
            <div className="thread-date">{t('今天', 'Today')}</div>
            {messages.map((message) => (
              <div key={message.id} className={`message ${message.role}`}>
                {message.role === 'assistant' && <Logo small />}
                <div className="message-body">
                  {message.role === 'assistant' && (
                    <div className="message-author">
                      Orangutan <span>{t('你的助手', 'Your assistant')}</span>
                    </div>
                  )}
                  <Markdown>{message.text}</Markdown>
                  {message.state === 'streaming' && <span className="stream-caret" />}
                  {message.state === 'interrupted' && (
                    <small className="muted">
                      {t('这次回复已中断', 'This response was interrupted')}
                    </small>
                  )}
                </div>
              </div>
            ))}
            {tasks.map((task) => (
              <div key={task.id} className="thread-task">
                <TaskCard task={task} />
                <Approval task={task} />
              </div>
            ))}
            {running && (
              <div className="thinking">
                <span className="thinking-dots">
                  <i />
                  <i />
                  <i />
                </span>
                {running.status === 'awaiting_approval'
                  ? t('等你确认后继续', 'Waiting for your decision')
                  : running.status === 'cancelling'
                    ? t('正在收好手头的工作', 'Wrapping up the current work')
                    : t('正在把事情理清楚…', 'Putting the pieces together…')}
              </div>
            )}
          </div>
        )}
      </div>
      {local ? (
        <div className="composer-wrap">
          <div className="composer" data-testid="composer">
            <textarea
              ref={textarea}
              aria-label={t('给助手发消息', 'Message your assistant')}
              placeholder={t('有什么想交给我的？', 'What can I help with?')}
              value={draft}
              rows={2}
              onChange={(e) =>
                dispatch({ type: 'draft', conversationId: conversation.id, text: e.target.value })
              }
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
                  suggest(
                    `${draft}${draft ? '\n' : ''}${t('请参考示例文件 meeting-notes.md，整理会议笔记。', 'Please organize the sample file meeting-notes.md.')}`,
                  );
                  toast(t('已添加示例资料', 'Sample notes attached'));
                }}
              >
                <Paperclip size={18} />
              </button>
              <label className="composer-model">
                <Sparkles size={13} />
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
                <span />
                {t('桌面对话', 'Desktop')}
              </span>
              {running ? (
                <button
                  className="send-button stop"
                  aria-label={t('停止本地任务', 'Stop local task')}
                  disabled={running.status === 'cancelling'}
                  onClick={() => dispatch({ type: 'stop', taskId: running.id, now: Date.now() })}
                >
                  <Square size={16} fill="currentColor" />
                </button>
              ) : (
                <button
                  className="send-button"
                  aria-label={t('发送消息', 'Send message')}
                  disabled={!draft.trim()}
                  onClick={send}
                >
                  <ArrowUp size={19} />
                </button>
              )}
            </div>
          </div>
          <div className="composer-caption">
            {t(
              '演示模式 · 回复与执行为模拟，资料保存在此浏览器',
              'Demo mode · Simulated replies and actions, saved in this browser',
            )}
            <span>Enter ↵</span>
          </div>
        </div>
      ) : (
        <div className="readonly-footer">
          <LockKeyhole size={16} />
          <div>
            <strong>
              {t('在这里看看，在原渠道继续。', 'View here. Continue in the original channel.')}
            </strong>
            <span>
              {t(
                '消息、任务与审批仅供预览，回复会回到发起请求的地方。',
                'Messages, tasks and approvals are read-only. Replies go back to where they started.',
              )}
            </span>
          </div>
        </div>
      )}
    </div>
  );
}
