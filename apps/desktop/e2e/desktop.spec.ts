import { test, expect } from '@playwright/test';

test('content-first home, read-only channel previews and notification routing', async ({
  page,
}) => {
  const errors: string[] = [];
  page.on('pageerror', (error) => errors.push(error.message));
  await page.goto('/');
  await expect(page.getByRole('heading', { name: '今天想处理什么？' })).toBeVisible();
  await page.screenshot({
    path: 'test-results/home-light.png',
    fullPage: true,
    animations: 'disabled',
  });
  await page.getByTestId('conversation-qq-main').click();
  await expect(page.getByTestId('composer')).toHaveCount(0);
  await expect(page.getByRole('button', { name: '允许这次操作' })).toHaveCount(0);
  await expect(page.getByText('渠道会话 · 只读预览')).toBeVisible();
  await page.getByTestId('task-card-qq-files').click();
  const panel = page.getByTestId('task-panel');
  await expect(panel.getByRole('button', { name: '停止任务' })).toHaveCount(0);
  await expect(panel.getByRole('button', { name: '重新尝试' })).toHaveCount(0);
  await expect(panel.getByText('渠道任务仅供预览')).toBeVisible();
  await page.getByRole('button', { name: '通知', exact: true }).click();
  const dialog = page.getByRole('dialog');
  await dialog.getByRole('button', { name: /找一个周末可以散步的地方/ }).click();
  await expect(panel.getByRole('heading', { name: '找一个周末可以散步的地方' })).toBeVisible();
  await expect(page.getByTestId('composer')).toHaveCount(0);
  await expect(page.locator('.artifact-paper [data-rich-text]')).toBeVisible();
  await page.screenshot({
    path: 'test-results/channel-workbench.png',
    fullPage: true,
    animations: 'disabled',
  });
  expect(errors).toEqual([]);
});

test('local approval, denial, retry and artifacts work through the interface', async ({ page }) => {
  await page.clock.install();
  await page.goto('/');
  await page.getByRole('textbox', { name: '给助手发消息' }).fill('帮我整理文件');
  await page.getByRole('button', { name: '发送消息' }).click();
  await page.clock.runFor(2400);
  await expect(page.getByRole('button', { name: '允许这次操作' })).toBeVisible();
  await page.getByRole('button', { name: '拒绝', exact: true }).click();
  await expect(page.getByText('你已拒绝此操作')).toBeVisible();
  await page.locator('.thread .task-card').click();
  await page.getByRole('button', { name: '重新尝试', exact: true }).click();
  await page.clock.runFor(2400);
  await page.getByTestId('task-panel').getByRole('button', { name: '允许这次操作' }).click();
  await page.clock.runFor(10_000);
  await page.getByTestId('task-panel').getByRole('button', { name: '成果', exact: true }).click();
  await expect(page.locator('.artifact-paper')).toContainText('帮我整理文件');
  await page.getByRole('button', { name: '变更', exact: true }).click();
  await expect(page.locator('.diff-preview')).toContainText('+ # 帮我整理文件');
});

test('close/reload interrupts runs, drafts survive appearance changes, both languages work', async ({
  page,
}) => {
  await page.clock.install();
  await page.goto('/');
  await page.getByRole('textbox', { name: '给助手发消息' }).fill('组织小队做一个计划');
  await page.getByRole('button', { name: '发送消息' }).click();
  await page.clock.runFor(800);
  await page.reload();
  await expect(page.locator('.thread .status')).toHaveText('已中断');
  await page.getByRole('textbox', { name: '给助手发消息' }).fill('还没发送的想法');
  await page.getByTestId('nav-settings').click();
  await page.getByTestId('theme-dark').click();
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
  await page.getByTestId('language-select').selectOption('en');
  await expect(page.getByRole('heading', { name: 'Settings', exact: true })).toBeVisible();
  await page.screenshot({
    path: 'test-results/settings-dark-en.png',
    fullPage: true,
    animations: 'disabled',
  });
  await page.getByTestId('nav-assistant').click();
  await expect(page.getByRole('textbox', { name: 'Message your assistant' })).toHaveValue(
    '还没发送的想法',
  );
  await page.emulateMedia({ colorScheme: 'dark', reducedMotion: 'reduce' });
  await page.getByTestId('nav-settings').click();
  await page.getByTestId('theme-system').click();
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
  await page.emulateMedia({ colorScheme: 'light' });
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'light');
});

test('memory edits, deletion, search, connection failure and retry are functional', async ({
  page,
}) => {
  await page.clock.install();
  await page.goto('/');
  await page.getByTestId('nav-memory').click();
  await page.getByRole('button', { name: /让资料各归其位/ }).click();
  await page.getByRole('textbox', { name: '标题', exact: true }).fill('清楚的工作区约定');
  await page.getByRole('button', { name: '保存记忆' }).click();
  await expect(page.getByRole('button', { name: /清楚的工作区约定/ })).toBeVisible();
  await page.getByRole('textbox', { name: '搜索记忆' }).fill('清楚的工作区');
  await expect(page.locator('.memory-card')).toHaveCount(1);
  await page.locator('.memory-card').click();
  await page.getByRole('button', { name: '删除', exact: true }).click();
  await page.getByRole('button', { name: '确认删除', exact: true }).click();
  await expect(page.locator('.memory-card')).toHaveCount(0);
  await page.getByTestId('nav-connections').click();
  const connection = page
    .locator('.connection-card')
    .filter({ has: page.getByRole('heading', { name: '飞书', exact: true }) });
  await connection.getByRole('button', { name: '配置', exact: true }).click();
  await page.getByRole('combobox', { name: '演示连接结果' }).selectOption('error');
  await page.getByRole('button', { name: '保存并模拟连接' }).click();
  await page.clock.runFor(800);
  await expect(connection.getByText('连接异常')).toBeVisible();
  await connection.getByRole('button', { name: '配置', exact: true }).click();
  await page.getByRole('textbox', { name: '示例账号名称' }).fill('Orangutan · 示例账号');
  await page.getByRole('combobox', { name: '演示连接结果' }).selectOption('success');
  await page.getByRole('button', { name: '保存并模拟连接' }).click();
  await page.clock.runFor(800);
  await expect(connection.getByText('已连接', { exact: true })).toBeVisible();
  await connection.getByRole('button', { name: '断开', exact: true }).click();
  await expect(connection.getByText('未连接', { exact: true })).toBeVisible();
});

test('narrow layout, search shortcut and dialog focus remain usable', async ({ page }) => {
  await page.setViewportSize({ width: 700, height: 850 });
  await page.goto('/');
  await page.getByRole('button', { name: '打开导航' }).click();
  await page.getByTestId('nav-tasks').click();
  await expect(page.getByRole('heading', { name: '任务', exact: true })).toBeVisible();
  await page.keyboard.press('Control+k');
  const search = page.getByRole('textbox', { name: '搜索关键词' });
  await expect(search).toBeFocused();
  await search.fill('周末');
  await page
    .getByRole('dialog')
    .getByRole('button', { name: /找一个周末可以散步的地方/ })
    .click();
  await expect(page.getByTestId('task-panel')).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await expect(page.locator('.artifact-paper [data-rich-text]')).toBeVisible();
  await page.screenshot({
    path: 'test-results/narrow-workbench.png',
    fullPage: true,
    animations: 'disabled',
  });
  await page.getByTestId('task-panel').getByRole('button', { name: '关闭', exact: true }).click();
  await page.getByRole('button', { name: '通知', exact: true }).click();
  await page.keyboard.press('Escape');
  await expect(page.getByRole('button', { name: '通知', exact: true })).toBeFocused();
});

test('corrupt storage is preserved and dangerous markdown is not executed', async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem('orangutan.desktop.demo', '{broken'));
  await page.goto('/');
  await expect(page.getByRole('alert')).toBeVisible();
  expect(await page.evaluate(() => localStorage.getItem('orangutan.desktop.demo'))).toBe('{broken');
  await page
    .getByRole('textbox', { name: '给助手发消息' })
    .fill('正常内容\n\n<script>window.__unsafe = true</script>\n\n[unsafe](javascript:alert(1))');
  await page.getByRole('button', { name: '发送消息' }).click();
  await expect(page.locator('.message.user [data-rich-text]')).toBeVisible();
  expect(await page.evaluate(() => '__unsafe' in window)).toBe(false);
  expect(await page.locator('a[href^="javascript:"]').count()).toBe(0);
});

test('page lifecycle stops demo effects and a cached page resumes interrupted', async ({
  page,
}) => {
  await page.clock.install();
  await page.goto('/');
  await page.getByRole('textbox', { name: '给助手发消息' }).fill('组织小队做一个计划');
  await page.getByRole('button', { name: '发送消息' }).click();
  await page.clock.runFor(800);
  const snapshot = () =>
    page.evaluate(() => JSON.parse(localStorage.getItem('orangutan.desktop.demo')!).tasks[0]);
  const before = await snapshot();
  await page.evaluate(() =>
    window.dispatchEvent(new PageTransitionEvent('pagehide', { persisted: true })),
  );
  await page.clock.runFor(6000);
  expect((await snapshot()).step).toBe(before.step);
  await page.evaluate(() =>
    window.dispatchEvent(new PageTransitionEvent('pageshow', { persisted: true })),
  );
  await expect(page.locator('.thread .status')).toHaveText('已中断');
  await page.clock.runFor(1500);
  expect((await snapshot()).status).toBe('interrupted');
});
