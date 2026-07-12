const cards = document.querySelector('#cards');
const template = document.querySelector('#card-template');
let books = [], polling, searchTimer, autoRefreshTimer;
let page = 1, totalPages = 1;

const viewMeta = {
  cards: ['我的单词', '浏览、复习并发送已保存的闪卡。'],
  create: ['新建闪卡', '从一个单词开始，做出专属的记忆卡。'],
  bulk: ['批量添加', '把一组新词一次整理进来。'],
  books: ['单词本', '用主题和课程把单词整理好。'],
  epd: ['EPD 自动刷新', '让墨水屏按设定的节奏展示闪卡。'],
};
function showView(view) {
  if (!viewMeta[view]) return;
  document.querySelectorAll('[data-view-panel]').forEach(panel => panel.classList.toggle('active', panel.dataset.viewPanel === view));
  document.querySelectorAll('[data-view]').forEach(item => item.classList.toggle('active', item.dataset.view === view));
  document.querySelector('#page-title').textContent = viewMeta[view][0];
  document.querySelector('#page-description').textContent = viewMeta[view][1];
  history.replaceState(null, '', `#${view}`);
}

const api = async (url, options = {}) => {
  const response = await fetch(url, options);
  if (!response.ok) throw new Error((await response.json().catch(() => ({}))).detail || '请求失败');
  return response.status === 204 ? null : response.json();
};
const escapeHtml = value => String(value).replace(/[&<>'"]/g, char => ({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'}[char]));
const selectedBook = () => document.querySelector('#book-filter').value;
const checkedIds = root => [...root.querySelectorAll('input[type=checkbox]:checked')].map(input => Number(input.value));
const pickerHtml = selected => books.map(book => `<label><input type="checkbox" value="${book.id}" ${selected.includes(book.id) ? 'checked' : ''}> ${escapeHtml(book.name)}</label>`).join('') || '<span class="help">请先新建单词本</span>';
function fillPicker(element, selected = []) { element.innerHTML = `<legend>${element.querySelector('legend')?.textContent || '单词本'}</legend>${pickerHtml(selected)}`; }

function refreshBookControls() {
  const filter = document.querySelector('#book-filter'), previous = filter.value;
  filter.innerHTML = '<option value="">全部单词本</option>' + books.map(book => `<option value="${book.id}">${escapeHtml(book.name)} (${book.card_count})</option>`).join('');
  filter.value = previous;
  const autoBook = document.querySelector('#auto-epd-book'), autoPrevious = autoBook.value;
  autoBook.innerHTML = '<option value="">全部单词</option>' + books.map(book => `<option value="${book.id}">${escapeHtml(book.name)}</option>`).join('');
  autoBook.value = autoPrevious;
  document.querySelectorAll('[data-book-picker]').forEach(picker => fillPicker(picker, checkedIds(picker)));
  document.querySelector('#book-list').innerHTML = books.length ? books.map(book => `<span class="tag">${escapeHtml(book.name)} <small>${book.card_count}</small> <button data-delete-book="${book.id}" title="删除单词本">×</button></span>`).join('') : '<span class="help">还没有单词本</span>';
  document.querySelectorAll('[data-delete-book]').forEach(button => button.onclick = async () => {
    if (!confirm('删除此单词本？仅解除关联，不会删除单词或其他单词本。')) return;
    await api(`/api/books/${button.dataset.deleteBook}`, {method: 'DELETE'});
    await loadBooks();
    page = 1;
    render();
  });
}
async function loadBooks() { books = await api('/api/books'); refreshBookControls(); }
function autoRefreshDetail(settings) {
  const scope = settings.book_name || '全部单词';
  const last = settings.last_card_word ? `上次发送：${escapeHtml(settings.last_card_word)}。` : '尚未发送。';
  const error = settings.last_error ? ` 最近错误：${escapeHtml(settings.last_error)}` : '';
  return `范围：${escapeHtml(scope)}，可发送 ${settings.eligible_cards} 张。${last}${error}`;
}
function renderAutoRefresh(settings) {
  const form = document.querySelector('#auto-epd-form');
  form.book_id.value = settings.book_id || '';
  form.interval_minutes.value = settings.interval_minutes;
  const state = document.querySelector('#auto-epd-state');
  state.textContent = settings.enabled ? '自动刷新中' : '已停止';
  state.className = `status ${settings.enabled ? 'ready' : ''}`;
  document.querySelector('#auto-epd-detail').innerHTML = autoRefreshDetail(settings);
}
async function loadAutoRefresh() { renderAutoRefresh(await api('/api/epd/auto-refresh')); }
async function setAutoRefresh(enabled) {
  const form = document.querySelector('#auto-epd-form');
  const settings = await api('/api/epd/auto-refresh', {method:'PUT', headers:{'Content-Type':'application/json'}, body:JSON.stringify({enabled, book_id:form.book_id.value ? Number(form.book_id.value) : null, interval_minutes:Number(form.interval_minutes.value)})});
  renderAutoRefresh(settings);
}
function statusText(card) { return {pending:'待生成', generating:'生成中', ready:'已就绪', failed:'生成失败'}[card.image_status] || '待生成'; }
function cardsUrl() {
  const params = new URLSearchParams({page, page_size: 24, sort: document.querySelector('#sort').value});
  if (selectedBook()) params.set('book_id', selectedBook());
  if (document.querySelector('#search').value.trim()) params.set('q', document.querySelector('#search').value.trim());
  return `/api/cards?${params}`;
}
function renderPagination(result) {
  totalPages = result.total_pages;
  document.querySelector('#page-info').textContent = `第 ${result.page} / ${result.total_pages} 页 · 共 ${result.total} 个`;
  document.querySelector('#previous').disabled = result.page <= 1;
  document.querySelector('#next').disabled = result.page >= result.total_pages;
}
async function render() {
  try {
    let result = await api(cardsUrl());
    if (result.page > result.total_pages) { page = result.total_pages; result = await api(cardsUrl()); }
    cards.innerHTML = '';
    if (!result.items.length) cards.innerHTML = '<p class="loading">没有符合条件的单词。</p>';
    result.items.forEach(card => {
      const node = template.content.cloneNode(true), article = node.querySelector('article');
      article.querySelector('h3').textContent = card.word;
      const status = article.querySelector('.status'); status.textContent = statusText(card); status.classList.add(card.image_status);
      article.querySelector('.pronunciation').textContent = `${card.syllables || '未填音节'} · ${card.ipa || '未填音标'}`;
      article.querySelector('.hint').textContent = card.hint || '未填记忆提示';
      article.querySelector('.book-tags').textContent = card.books.map(book => book.name).join(' · ') || '未加入单词本';
      article.querySelector('.review').textContent = `已复习 ${card.review_count} 次${card.image_error ? ` · ${card.image_error}` : ''}`;
      const picker = article.querySelector('.book-picker');
      fillPicker(picker, card.books.map(book => book.id));
      picker.onchange = async () => { try { await api(`/api/cards/${card.id}/books`, {method:'PUT', headers:{'Content-Type':'application/json'}, body:JSON.stringify({book_ids:checkedIds(picker)})}); await loadBooks(); render(); } catch(error) { alert(error.message); }};
      const preview = article.querySelector('.preview');
      preview.innerHTML = card.image_url ? `<img src="${card.image_url}?v=${Date.now()}" alt="${escapeHtml(card.word)} flash card">` : `<span>${statusText(card)}</span>`;
      article.querySelector('.generate').onclick = () => action(`/api/cards/${card.id}/generate`, '已重新生成图片');
      article.querySelector('.review-btn').onclick = () => action(`/api/cards/${card.id}/review`, '已记录复习');
      article.querySelector('.epd').onclick = () => action(`/api/cards/${card.id}/epd`, '已发送至 EPD');
      article.querySelector('.delete').onclick = async () => { if (confirm(`删除 ${card.word}？`)) { await api(`/api/cards/${card.id}`, {method:'DELETE'}); await loadBooks(); render(); }};
      cards.append(node);
    });
    renderPagination(result);
    await progress();
  } catch (error) { cards.innerHTML = `<p class="error">${escapeHtml(error.message)}</p>`; }
}
async function progress() {
  try {
    const suffix = selectedBook() ? `?book_id=${selectedBook()}` : '';
    const result = await api('/api/images/progress' + suffix), percent = result.total ? Math.round(result.ready / result.total * 100) : 0;
    document.querySelector('.bar i').style.width = `${percent}%`;
    document.querySelector('#progress-label').textContent = `${result.ready}/${result.total} 已生成 · ${result.generating} 生成中 · ${result.failed} 失败`;
    clearTimeout(polling); if (result.running || result.generating) polling = setTimeout(render, 2000);
  } catch (_) {}
}
async function action(url, success) { try { await api(url, {method:'POST'}); await render(); alert(success); } catch(error) { alert(error.message); } }

document.querySelector('#book-form').onsubmit = async event => { event.preventDefault(); try { await api('/api/books', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(Object.fromEntries(new FormData(event.target)))}); event.target.reset(); await loadBooks(); } catch(error) { alert(error.message); }};
document.querySelector('#auto-epd-form').onsubmit = async event => { event.preventDefault(); try { await setAutoRefresh(true); } catch(error) { alert(error.message); }};
document.querySelector('#auto-epd-stop').onclick = async () => { try { await setAutoRefresh(false); } catch(error) { alert(error.message); }};
document.querySelector('#enrich').onclick = async () => { const form = document.querySelector('#create-form'), word = form.word.value.trim(); if (!word) return alert('请先输入单词'); try { const data = await api('/api/cards/enrich', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({word})}); form.ipa.value = data.ipa; form.syllables.value = data.syllables; form.hint.value = data.hint; } catch(error) { alert(error.message); }};
document.querySelector('#create-form').onsubmit = async event => { event.preventDefault(); const form = event.target, payload = Object.fromEntries(new FormData(form)); payload.book_ids = checkedIds(form.querySelector('[data-book-picker]')); try { await api('/api/cards', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload)}); form.reset(); fillPicker(form.querySelector('[data-book-picker]')); page = 1; await loadBooks(); render(); } catch(error) { alert(error.message); }};
document.querySelector('#bulk-form').onsubmit = async event => { event.preventDefault(); const form = event.target, payload = {words:form.words.value, book_ids:checkedIds(form.querySelector('[data-book-picker]'))}; try { const result = await api('/api/cards/bulk', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload)}); form.reset(); fillPicker(form.querySelector('[data-book-picker]')); document.querySelector('#bulk-result').textContent = `已新增 ${result.created_count} 项，后台正在自动补全；跳过 ${result.skipped.length} 项${result.skipped.length ? `（${result.skipped.slice(0, 3).map(item => item.word).join('、')}）` : ''}`; page = 1; await loadBooks(); render(); } catch(error) { alert(error.message); }};
document.querySelector('#batch').onclick = async () => { try { const suffix = selectedBook() ? `?book_id=${selectedBook()}` : ''; const result = await api('/api/images/generate-batch' + suffix, {method:'POST'}); alert(`已在后台排队 ${result.queued} 张图片`); render(); } catch(error) { alert(error.message); }};
document.querySelector('#book-filter').onchange = () => { page = 1; render(); };
document.querySelector('#sort').onchange = () => { page = 1; render(); };
document.querySelector('#search').oninput = () => { clearTimeout(searchTimer); searchTimer = setTimeout(() => { page = 1; render(); }, 250); };
document.querySelector('#previous').onclick = () => { if (page > 1) { page--; render(); }};
document.querySelector('#next').onclick = () => { if (page < totalPages) { page++; render(); }};
document.querySelector('#refresh').onclick = render;
document.querySelectorAll('[data-view]').forEach(item => item.onclick = () => showView(item.dataset.view));
document.querySelectorAll('[data-view-link]').forEach(item => item.onclick = event => { event.preventDefault(); showView(item.dataset.viewLink); });
(async () => { showView(location.hash.slice(1) || 'cards'); await loadBooks(); await loadAutoRefresh(); render(); autoRefreshTimer = setInterval(() => loadAutoRefresh().catch(() => {}), 10000); })();
