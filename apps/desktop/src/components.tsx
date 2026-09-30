import { lazy, Suspense, useEffect, useRef, type ReactNode } from 'react';
import {
  ArrowUpRight,
  Check,
  Circle,
  Clock3,
  LoaderCircle,
  LockKeyhole,
  Send,
  Sparkles,
  X,
  CircleAlert,
  CheckCheck,
  Radio,
} from 'lucide-react';
import { useApp } from './context.ts';
import type { Status, Task } from './schema.ts';
import { active, canControl } from './model.ts';

export function Logo({ small = false }: { small?: boolean }) {
  return <img src="/orangutan.svg" className={small ? 'logo small' : 'logo'} alt="Orangutan" />;
}

export function PlatformIcon({ platform, size = 18 }: { platform: string; size?: number }) {
  if (platform === 'telegram')
    return (
      <span className="platform telegram" style={{ width: size + 12, height: size + 12 }}>
        <Send size={size} />
      </span>
    );
  if (platform === 'qq')
    return (
      <span className="platform qq" style={{ width: size + 12, height: size + 12 }}>
        <svg width={size} height={size} viewBox="0 0 24 24" aria-hidden="true">
          <path
            d="M7 10C5 3 19 3 17 10c3 3 4 9 2 8l-2-1c0 4-10 4-10 0l-2 1c-2 1-1-5 2-8Z"
            fill="currentColor"
          />
          <path
            d="M8 13h8m-7 6-2 2m8-2 2 2"
            stroke="#edb57d"
            strokeWidth="2"
            strokeLinecap="round"
          />
          <circle cx="10" cy="9" r="1" fill="white" />
          <circle cx="14" cy="9" r="1" fill="white" />
        </svg>
      </span>
    );
  if (platform === 'feishu')
    return (
      <span className="platform feishu" style={{ width: size + 12, height: size + 12 }}>
        <svg width={size} height={size} viewBox="0 0 24 24" fill="none" aria-hidden="true">
          <path d="m3 5 10 7-5 5-5-2V5Z" fill="#4285db" />
          <path d="m7 19 8-8 6-3v8l-8 5-6-2Z" fill="#4eb2bd" />
        </svg>
      </span>
    );
  return (
    <span className="platform desktop">
      <Sparkles size={size} />
    </span>
  );
}

const labels: Record<Status, [string, string]> = {
  queued: ['准备中', 'Queued'],
  running: ['进行中', 'Working'],
  awaiting_approval: ['待确认', 'Needs approval'],
  cancelling: ['正在停止', 'Stopping'],
  completed: ['已完成', 'Completed'],
  failed: ['遇到问题', 'Needs attention'],
  cancelled: ['已停止', 'Stopped'],
  interrupted: ['已中断', 'Interrupted'],
};
export function StatusBadge({ status }: { status: Status }) {
  const { t } = useApp();
  const Icon =
    status === 'completed'
      ? Check
      : status === 'awaiting_approval'
        ? Clock3
        : status === 'failed'
          ? CircleAlert
          : active(status)
            ? LoaderCircle
            : Circle;
  return (
    <span className={`status ${status}`}>
      <Icon size={12} className={status === 'running' || status === 'cancelling' ? 'spin' : ''} />
      {t(...labels[status])}
    </span>
  );
}

const RichText = lazy(() => import('./RichText.tsx'));
export function Markdown({ children }: { children: string }) {
  return (
    <div className="markdown">
      <Suspense
        fallback={
          <div className="rich-text-loading" aria-busy="true">
            <i />
            <i />
            <i />
          </div>
        }
      >
        <RichText>{children}</RichText>
      </Suspense>
    </div>
  );
}

export function Modal({
  title,
  children,
  close,
  wide = false,
}: {
  title: string;
  children: ReactNode;
  close: () => void;
  wide?: boolean;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const previousFocus = useRef(document.activeElement as HTMLElement | null);
  useEffect(() => {
    const element = dialog.current;
    element?.showModal();
    element?.querySelector<HTMLElement>('input, textarea, [data-autofocus]')?.focus();
    return () => {
      element?.close();
      if (previousFocus.current?.isConnected) previousFocus.current.focus();
    };
  }, []);
  return (
    <dialog
      ref={dialog}
      className={`modal ${wide ? 'wide' : ''}`}
      aria-label={title}
      onCancel={(e) => {
        e.preventDefault();
        close();
      }}
      onClick={(e) => {
        if (e.target === e.currentTarget) close();
      }}
    >
      <div className="modal-header">
        <h2>{title}</h2>
        <CloseButton onClick={close} />
      </div>
      {children}
    </dialog>
  );
}

export function CloseButton({ onClick }: { onClick: () => void }) {
  const { t } = useApp();
  return (
    <button className="icon-button" aria-label={t('关闭', 'Close')} onClick={onClick}>
      <X size={18} />
    </button>
  );
}

export function Empty({
  title,
  detail,
  icon,
}: {
  title: string;
  detail: string;
  icon?: ReactNode;
}) {
  return (
    <div className="empty-state">
      {icon ?? <Radio size={28} />}
      <h3>{title}</h3>
      <p>{detail}</p>
    </div>
  );
}

export function Source({ conversationId }: { conversationId: string }) {
  const { state, t } = useApp();
  const c = state.conversations.find((c) => c.id === conversationId);
  const connection = state.connections.find((v) => v.id === c?.connectionId);
  return (
    <span className="source">
      <span className={`source-dot ${connection?.platform ?? 'desktop'}`} />
      {c?.origin === 'desktop' ? t('桌面', 'Desktop') : `${connection?.name} · ${c?.name}`}
    </span>
  );
}

export function TaskCard({ task }: { task: Task }) {
  const { state, t, openTask } = useApp();
  return (
    <button
      className="task-card"
      onClick={() => openTask(task.id)}
      data-testid={`task-card-${task.id}`}
    >
      <div className="task-card-top">
        <span className="task-type">
          <Sparkles size={14} />
          {task.members.length
            ? t('小队协作', 'A little teamwork')
            : t('任务进展', 'Task progress')}
        </span>
        <StatusBadge status={task.status} />
      </div>
      <strong>{task.title}</strong>
      {task.members.length > 0 && (
        <div className="team-mini">
          <span className="stacked-avatars">
            {task.members.map((m, i) => (
              <span key={m.id} className={`member-avatar color-${i}`}>
                {m.name[0]}
              </span>
            ))}
          </span>
          <span>{task.members.map((m) => m.name).join(' · ')}</span>
        </div>
      )}
      <div className="task-card-bottom">
        <span>
          {task.artifact ? task.artifact.name : t('查看执行步骤与详情', 'View steps and details')}
        </span>
        {canControl(state, task.conversationId) ? (
          <ArrowUpRight size={15} />
        ) : (
          <LockKeyhole size={13} />
        )}
      </div>
    </button>
  );
}

export function Approval({ task }: { task: Task }) {
  const { state, dispatch, t } = useApp();
  if (!task.approval) return null;
  const local = canControl(state, task.conversationId);
  const pending = task.approval.status === 'pending';
  const approvalLabels = {
    pending: t('需要确认一个文件操作', 'A file action needs approval'),
    approved: t('已允许此操作', 'Action approved'),
    rejected: t('你已拒绝此操作', 'Action declined'),
    expired: t('此确认请求已失效', 'This request has expired'),
    cancelled: t('此确认请求已取消', 'This request was cancelled'),
  };
  return (
    <div className={`approval-card ${pending ? '' : 'settled'}`} data-testid="approval-card">
      <div className="approval-heading">
        {pending ? <LockKeyhole size={17} /> : <CheckCheck size={17} />}
        <strong>{approvalLabels[task.approval.status]}</strong>
      </div>
      <p>
        {t(
          '写入整理后的笔记，保留原始文件。',
          'Write the organized note and keep the original files.',
        )}
      </p>
      <code>{task.approval.target}</code>
      {pending && local && (
        <div className="approval-actions">
          <button
            className="button secondary"
            onClick={() =>
              dispatch({ type: 'reject', taskId: task.id, approvalId: task.approval!.id })
            }
          >
            {t('拒绝', 'Decline')}
          </button>
          <button
            className="button primary"
            disabled={state.settings.permission === 'deny'}
            onClick={() =>
              dispatch({ type: 'approve', taskId: task.id, approvalId: task.approval!.id })
            }
          >
            {t('允许这次操作', 'Allow this action')}
          </button>
        </div>
      )}
      {!local && (
        <div className="read-only-note">
          <LockKeyhole size={12} />
          {t('仅供预览 · 请在原渠道处理', 'Preview only · Respond in the original channel')}
        </div>
      )}
    </div>
  );
}

export function PlantIllustration() {
  return (
    <svg className="plant-illustration" viewBox="0 0 210 190" fill="none" aria-hidden="true">
      <ellipse cx="111" cy="169" rx="69" ry="9" fill="var(--illustration-shadow)" />
      <circle cx="112" cy="87" r="65" fill="var(--illustration-halo)" />
      <path
        d="M103 153c-4-31 1-61 17-97m-15 65c-17-15-27-34-31-56m32 38c22-7 34-19 40-37"
        stroke="#687c58"
        strokeWidth="3"
        strokeLinecap="round"
      />
      <path d="M119 69c-8-24 2-42 18-43 6 17 0 32-18 43Z" fill="#83966e" />
      <path d="M109 100c-18-22-15-39-3-48 16 12 17 31 3 48Z" fill="#a2af8a" />
      <path d="M86 91c-23-5-31-18-25-33 17-1 30 12 25 33Z" fill="#879675" />
      <path d="M126 91c4-26 20-38 38-32-2 19-15 33-38 32Z" fill="#adb895" />
      <path d="M102 129c-30-4-44-15-41-32 25-4 42 11 41 32Z" fill="#a7b68c" />
      <path d="M87 139h40l-6 30H94l-7-30Z" fill="#ca8664" />
      <path d="M86 140h42" stroke="#aa6a4c" strokeWidth="4" strokeLinecap="round" />
      <path d="m144 144 32-4 3 22-32 4-3-22Z" fill="#e3cfaa" />
      <path d="m143 148 34-4m-27 14 21-3" stroke="#a69069" strokeWidth="1.5" />
      <path d="M45 153h28v11H45z" fill="#d4b89b" />
      <path d="M46 153h26" stroke="#8c775d" strokeWidth="2" />
      <path d="M50 146h26v7H50z" fill="#99a98a" />
      <path
        d="m43 72 3-7m-3 7-8-1m135 29 5 3m-5-3 1-7"
        stroke="#c79861"
        strokeWidth="2"
        strokeLinecap="round"
      />
    </svg>
  );
}
