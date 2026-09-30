import { useEffect, useRef, useState } from 'react';
import {
  Check,
  ChevronRight,
  FileText,
  GitBranch,
  LockKeyhole,
  RotateCcw,
  Square,
  Users,
} from 'lucide-react';
import { useApp } from './context.ts';
import { active, canControl } from './model.ts';
import { Approval, CloseButton, Markdown, Source, StatusBadge } from './components.tsx';

export function TaskPanel({ id, close }: { id: string; close: () => void }) {
  const { state, dispatch, t } = useApp();
  const task = state.tasks.find((task) => task.id === id);
  const [tab, setTab] = useState<'result' | 'progress' | 'team'>(
    task?.artifact ? 'result' : 'progress',
  );
  const [raw, setRaw] = useState(false);
  const [diff, setDiff] = useState(false);
  const panel = useRef<HTMLElement>(null);
  const previousFocus = useRef(document.activeElement as HTMLElement | null);
  useEffect(() => {
    panel.current?.querySelector<HTMLButtonElement>('button')?.focus();
    return () => {
      const previous = previousFocus.current;
      if (previous?.isConnected && previous.getClientRects().length) previous.focus();
      else document.getElementById('main-content')?.focus();
    };
  }, []);
  if (!task) return null;
  const local = canControl(state, task.conversationId);
  const stages = [
    t('理解请求', 'Understand the request'),
    t('查阅相关资料', 'Gather the context'),
    t('执行与检查', 'Work and review'),
    t('整理成果', 'Bring it together'),
  ];
  const stage = task.status === 'completed' ? 4 : Math.min(3, Math.floor(task.step / 2));
  return (
    <aside
      ref={panel}
      className="detail-panel"
      aria-label={t('任务详情', 'Task details')}
      data-testid="task-panel"
    >
      <div className="panel-header">
        <span>
          <FileText size={16} />
          {t('任务详情', 'Task details')}
        </span>
        <CloseButton onClick={close} />
      </div>
      <div className="panel-title">
        <Source conversationId={task.conversationId} />
        <h2>{task.title}</h2>
        <StatusBadge status={task.status} />
      </div>
      <div className="panel-tabs">
        <button className={tab === 'result' ? 'active' : ''} onClick={() => setTab('result')}>
          <FileText size={14} />
          {t('成果', 'Result')}
        </button>
        <button className={tab === 'progress' ? 'active' : ''} onClick={() => setTab('progress')}>
          <GitBranch size={14} />
          {t('进度', 'Progress')}
        </button>
        {task.members.length > 0 && (
          <button className={tab === 'team' ? 'active' : ''} onClick={() => setTab('team')}>
            <Users size={14} />
            {t('小队', 'Team')}
          </button>
        )}
      </div>
      <div className="panel-content">
        {tab === 'result' &&
          (task.artifact ? (
            <>
              <div className="artifact-toolbar">
                <span>{task.artifact.name}</span>
                <button
                  onClick={() => {
                    setRaw(!raw);
                    setDiff(false);
                  }}
                >
                  {raw ? t('预览', 'Preview') : t('原文', 'Source')}
                </button>
                {task.kind === 'file' && (
                  <button
                    onClick={() => {
                      setDiff(!diff);
                      setRaw(false);
                    }}
                  >
                    {t('变更', 'Changes')}
                  </button>
                )}
              </div>
              <article className="artifact-paper">
                {diff ? (
                  <pre className="diff-preview">
                    {task.artifact.body
                      .split('\n')
                      .map((line) => `+ ${line}`)
                      .join('\n')}
                  </pre>
                ) : raw ? (
                  <pre>{task.artifact.body}</pre>
                ) : (
                  <Markdown>{task.artifact.body}</Markdown>
                )}
              </article>
              <div className="artifact-footnote">
                <span className="source-dot desktop" />
                {t('演示工作区 · 仅供预览', 'Demo workspace · Preview only')}
              </div>
            </>
          ) : (
            <div className="panel-empty">
              <FileText size={32} />
              <h3>{t('暂无成果', 'No artifact yet')}</h3>
              <p>
                {t(
                  '任务完成后，可以在这里查看文档、文件和整理好的方案。',
                  'Documents, files and plans will appear here when the task is finished.',
                )}
              </p>
            </div>
          ))}
        {tab === 'progress' && (
          <>
            <div className="execution-steps">
              {stages.map((label, i) => (
                <div
                  className={`execution-step ${i < stage ? 'done' : i === stage && active(task.status) ? 'current' : ''}`}
                  key={label}
                >
                  <span>{i < stage ? <Check size={13} /> : i + 1}</span>
                  <div>
                    <strong>{label}</strong>
                    <p>
                      {i < stage
                        ? t('已完成', 'Completed')
                        : i === stage && active(task.status)
                          ? t('正在处理', 'In progress')
                          : t('等待前一步完成', 'Waiting for the previous step')}
                    </p>
                  </div>
                </div>
              ))}
            </div>
            <Approval task={task} />
            {task.status === 'failed' && (
              <div className="error-note">
                {task.approval?.status === 'rejected'
                  ? t(
                      '操作已被拒绝，未产生文件变更。',
                      'The action was declined. No file change was made.',
                    )
                  : t(
                      '示例执行未完成。可以查看小队记录，或在所属入口重新发起。',
                      'The demo run did not complete. Review the team notes or retry from its own channel.',
                    )}
              </div>
            )}
            {task.status === 'interrupted' && (
              <div className="error-note">
                {t(
                  '上次页面关闭时，这项工作还没有完成。历史已保留，未自动重新执行。',
                  'This work was unfinished when the page closed. History is kept; it has not been restarted.',
                )}
              </div>
            )}
            {task.history.length > 0 && (
              <div className="attempt-history">
                <h4>{t('之前的执行', 'Previous attempts')}</h4>
                {task.history.map((h) => (
                  <div key={h.attempt}>
                    <span>#{h.attempt}</span>
                    <StatusBadge status={h.status} />
                  </div>
                ))}
              </div>
            )}
          </>
        )}
        {tab === 'team' && (
          <>
            <p className="panel-intro">
              {t(
                '成员按任务分工，Orangutan 负责检查和汇总结果。',
                'Members work on assigned tasks. Orangutan reviews and consolidates their results.',
              )}
            </p>
            <div className="team-members">
              {task.members.map((member) => (
                <details className="team-member" key={member.id} open={member.state === 'failed'}>
                  <summary>
                    <span className="member-avatar">{member.name[0]}</span>
                    <div>
                      <strong>{member.name}</strong>
                      <span>{member.role}</span>
                    </div>
                    <span className={`member-dot ${member.state}`} />
                    <ChevronRight size={14} />
                  </summary>
                  <p>
                    {member.result ||
                      t(
                        '收到分工后，工作记录会显示在这里。',
                        'Work notes appear here after the assignment begins.',
                      )}
                  </p>
                  <span className="member-state">
                    {
                      {
                        waiting: t('等待中', 'Waiting'),
                        working: t('工作中', 'Working'),
                        done: t('已完成', 'Done'),
                        failed: t('遇到问题', 'Needs attention'),
                        stopped: t('已停止', 'Stopped'),
                      }[member.state]
                    }
                  </span>
                </details>
              ))}
            </div>
            <div className="team-leader">
              <img src="/orangutan.svg" alt="" />
              <div>
                <strong>Orangutan</strong>
                <span>{t('负责协调、检查与汇总', 'Coordinates, reviews and delivers')}</span>
              </div>
            </div>
          </>
        )}
      </div>
      <div className="panel-footer">
        {!local ? (
          <span>
            <LockKeyhole size={13} />
            {t('渠道任务仅供预览', 'Channel task · Read only')}
          </span>
        ) : (
          <>
            <span>{t('本地工作区', 'Local workspace')}</span>
            {active(task.status) ? (
              <button
                className="button secondary small-button"
                disabled={task.status === 'cancelling'}
                onClick={() => dispatch({ type: 'stop', taskId: task.id, now: Date.now() })}
              >
                <Square size={12} />
                {t('停止任务', 'Stop task')}
              </button>
            ) : ['failed', 'cancelled', 'interrupted'].includes(task.status) ? (
              <button
                className="button secondary small-button"
                onClick={() => dispatch({ type: 'retry', taskId: task.id, now: Date.now() })}
              >
                <RotateCcw size={12} />
                {t('重新尝试', 'Try again')}
              </button>
            ) : (
              <span>
                <Check size={12} />
                {t('已完成', 'Completed')}
              </span>
            )}
          </>
        )}
      </div>
    </aside>
  );
}
