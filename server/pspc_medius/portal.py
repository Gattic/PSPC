# SPDX-License-Identifier: GPL-2.0-or-later
"""The Community pages: what the game's online menu shows under Community (the SVO browser opens
homeURI there). Leaderboards, agent and cell profiles, the message board, mission charts and news,
as SVML pages.

SVML as this game's browser reads it (worked out from the game): every visible tag is a direct
child of <SVML> except form fields, which go inside their FORM; plain 480x272 pixel coordinates
(the Community tab's header covers y < 38, its button bar y > 243); colors #AARRGGBB; TEXT is one
unwrapped line (at most 127 characters) and multi-line text is a read-only TEXTAREA; a GRID needs
numCols / numTotalRows / numVisRows / colWidth / rowHeight and exactly numCols COLUMNs; at most 256
tags per page; only &amp; &lt; &gt; &quot; &apos; as entities. Cross follows links, Circle goes
back a page, SELECT goes home.
"""

import html
import logging
import time

from . import records, stats
from .http import Response

log = logging.getLogger('medius.portal')

GOLD, WHITE, GREY, RED, BLACK = '#FFFFD200', '#FFFFFFFF', '#FFB4B4B4', '#FFC81E1E', '#FF000000'
PANEL, LINE_C, ROW_FILL, BTN_FILL = '#C8000000', '#FF8C1E1E', '#B4141414', '#FF3C0A0A'
TOP, BOTTOM = 40, 243
ROWS_PER_PAGE = 9
BOARDS = [('general', 'General'), ('games', 'Looking for a Game'), ('cells', 'Cells & Recruiting'),
          ('missions', 'Mission Makers')]
POST_INTERVAL = 30            # seconds between a player's posts
POSTS_PER_DAY = 50


def esc(text):
    """Text as the game's XML parser reads it: its five entities, ASCII only."""
    s = str(text).encode('ascii', 'replace').decode('ascii')
    return html.escape(s, quote=True).replace('&#x27;', '&apos;')


def _int(q, name, default=0):
    try:
        return int(q.get(name, default))
    except (TypeError, ValueError):
        return default


def ago(t):
    d = max(0, int(time.time()) - int(t))
    for size, unit in ((86400, 'd'), (3600, 'h'), (60, 'm')):
        if d >= size:
            return '%d%s ago' % (d // size, unit)
    return 'just now'


class Page:
    """An SVML page under construction (tags in drawing order)."""

    def __init__(self, title=None, page_id=None):
        self.tags = []
        self.names = 0
        if page_id:
            self.tags.append('<PAGEID name="%s"/>' % page_id)
        if title:
            self.text(20, TOP, title, size=20, color=GOLD)
            self.line(20, TOP + 22, 460, TOP + 22)

    def name(self, prefix):
        self.names += 1
        return '%s%d' % (prefix, self.names)

    @staticmethod
    def attrs(d):
        return ''.join(' %s="%s"' % (k, esc(v)) for k, v in d.items() if v is not None)

    def text(self, x, y, s, size=14, color=WHITE, align='left', width=None):
        self.tags.append('<TEXT%s>%s</TEXT>' % (self.attrs({
            'name': self.name('t'), 'x': x, 'y': y, 'fontSize': size, 'textColor': color, 'align': align,
            'displayLength': width}), esc(str(s)[:120])))

    def rect(self, x, y, w, h, fill=PANEL, line=LINE_C, thickness=1):
        self.tags.append('<RECTANGLE%s/>' % self.attrs({
            'name': self.name('r'), 'x': x, 'y': y, 'width': w, 'height': h, 'fillColor': fill, 'lineColor': line,
            'lineThickness': thickness}))

    def line(self, x, y, x2, y2, color=LINE_C):
        self.tags.append('<LINE%s/>' % self.attrs({'name': self.name('l'), 'x': x, 'y': y, 'endX': x2, 'endY': y2,
                                                  'thickness': 1, 'lineColor': color}))

    def button(self, x, y, w, h, label, href=None, name=None, default=False, size=14, **extra):
        a = {'name': name or self.name('b'), 'x': x, 'y': y, 'width': w, 'height': h, 'href': href, 'fontSize': size,
             'fillColor': BTN_FILL, 'lineColor': LINE_C, 'textColor': WHITE, 'highlightFillColor': GOLD,
             'highlightLineColor': WHITE, 'highlightTextColor': BLACK}
        if default:
            a['selectable'] = 'default'
        elif href is None:
            a['selectable'] = 'true'
        a.update(extra)
        self.tags.append('<BUTTON%s>%s</BUTTON>' % (self.attrs(a), esc(str(label)[:120])))
        return a['name']

    def textarea(self, x, y, w, h, s, lines, scroll=True, spacing=16):
        self.tags.append('<TEXTAREA%s>%s</TEXTAREA>' % (self.attrs({
            'name': self.name('a'), 'x': x, 'y': y, 'width': w, 'height': h, 'readonly': 'true',
            'selectable': 'true' if scroll else None, 'linesVisible': lines, 'lineSpacing': spacing,
            'topPadValue': 3, 'leftPadValue': 4, 'fillColor': PANEL, 'lineColor': LINE_C, 'textColor': WHITE,
            'highlightFillColor': PANEL, 'highlightLineColor': GOLD, 'highlightTextColor': WHITE,
            'maxLength': 12000}), esc(s)))

    def grid(self, x, y, columns, rows, mine=None, visible=ROWS_PER_PAGE, row_height=15):
        """columns: [(heading, width, align)]; rows: [[text or (text, href)]]; mine: an extra row
        (your own standing) drawn under the others."""
        if not rows:
            rows = [['' for _ in columns]]
            rows[0][min(1, len(columns) - 1)] = 'Nobody yet'
        cols = ''.join('<COLUMN colWidth="%d" align="%s">%s</COLUMN>' % (w, a, esc(h)) for h, w, a in columns)

        def cells(row):
            out = []
            for c in row:
                text, href = c if isinstance(c, tuple) else (c, None)
                text = esc(text) if str(text) != '' else ' '
                out.append('<CELL%s>%s</CELL>' % (' href="%s"' % esc(href) if href else '', text))
            return ''.join(out)
        body = ''.join('<ROW>%s</ROW>' % cells(r) for r in rows)
        if mine:
            body += '<MY_RECORD_ROW textColor="%s" spaceBelowBottomRow="2">%s</MY_RECORD_ROW>' % (GOLD, cells(mine))
        modal = any(isinstance(c, tuple) and c[1] for r in rows for c in r)
        self.tags.append(
            '<GRID%s><COLUMNS height="16" fontSize="14" fillColor="#FF500A0A" lineColor="%s" textColor="%s">%s</COLUMNS>'
            '<ROWS>%s</ROWS></GRID>' % (self.attrs({
                'name': self.name('g'), 'x': x, 'y': y, 'numCols': len(columns), 'numTotalRows': len(rows),
                'numVisRows': min(visible, len(rows)), 'colWidth': 60, 'rowHeight': row_height, 'cellFontSize': 14,
                'cellAlign': 'center', 'cellFillColor': ROW_FILL, 'cellLineColor': ROW_FILL, 'cellTextColor': WHITE,
                'cellHighlightFillColor': GOLD, 'cellHighlightLineColor': GOLD, 'cellHighlightTextColor': BLACK,
                'GridIsModal': 'true' if modal else None}), LINE_C, GOLD, cols, body))

    def raw(self, tag):
        self.tags.append(tag)

    def render(self):
        return Response(200, '<?xml version="1.0" encoding="UTF-8"?>\n<SVML>\n%s\n</SVML>\n' % '\n'.join(self.tags[:256]),
                        'text/svml')


class Portal:
    def __init__(self, server, svo):
        self.server = server
        self.svo = svo

    @property
    def store(self):
        return self.server.store

    @property
    def records(self):
        return self.server.records

    def routes(self):
        return {'home.svml': self.home, 'news.svml': self.news, 'lb.svml': self.leaderboard, 'agent.svml': self.agent,
                'find.svml': self.find, 'cells.svml': self.cells, 'cell.svml': self.cell, 'board.svml': self.boards,
                'topics.svml': self.topics, 'topic.svml': self.topic, 'post.svml': self.compose,
                'board/submit': self.submit, 'missions.svml': self.missions, 'mission.svml': self.mission}

    def url(self, path, **query):
        q = '&'.join('%s=%s' % (k, v) for k, v in query.items() if v is not None)
        return self.svo.uri(path) + ('?' + q if q else '')

    def viewer(self, req):
        return self.svo.missions.account_of(req)

    def pager(self, page, path, p, more, **query):
        """Prev / Next buttons along the bottom."""
        if p > 0:
            page.button(20, 224, 90, 18, '< Prev', self.url(path, p=p - 1, **query))
        if more:
            page.button(370, 224, 90, 18, 'Next >', self.url(path, p=p + 1, **query))

    # ---- home and news

    def home(self, req):
        lobby = self.server.lobby
        page = Page(self.server.game.universe_name, 'home')
        playing = sum(1 for g in lobby.games.values() if g.players)
        page.text(460, TOP + 4, '%d online, %d games' % (lobby.player_count(), playing), color=GREY, align='right')
        latest = self.store.news(1)
        page.textarea(20, TOP + 28, 440, 40, latest[0]['text'] if latest else self.server.game.news, 2, scroll=False)
        items = [('Leaderboards', 'lb.svml'), ('Cell Standings', 'cells.svml'), ('Message Board', 'board.svml'),
                 ('Mission Charts', 'missions.svml'), ('Find an Agent', 'find.svml'), ('News', 'news.svml')]
        for i, (label, path) in enumerate(items):
            col, row = i % 2, i // 2
            page.button(40 + col * 210, TOP + 80 + row * 38, 190, 28, label, self.svo.uri(path), default=i == 0)
        me = self.records.player(self.viewer(req)) if self.viewer(req) else None
        if me:
            page.text(240, TOP + 196, 'You: %s  %d XP  %d wins in %d games' % (me['rank'], me['xp'], me['wins'],
                                                                                me['games']), color=GREY, align='center')
        return page.render()

    def news(self, req):
        page = Page('News', 'news')
        items = self.store.news(20)
        text = '\n\n'.join('%s  %s' % (time.strftime('%d %b %Y', time.gmtime(n['created'])), n['text']) for n in items)
        page.textarea(20, TOP + 28, 440, 170, text or 'No news yet.', 10)
        return page.render()

    # ---- players

    def leaderboard(self, req):
        q = req.query
        keys = list(records.PLAYER_BOARDS)
        key = q.get('b') if q.get('b') in records.PLAYER_BOARDS else 'xp'
        p = max(0, _int(q, 'p'))
        title, _, heading, _ = records.PLAYER_BOARDS[key]
        board = self.records.board(key)
        page = Page(title, 'leaderboard')
        i = keys.index(key)
        prev, nxt = keys[i - 1], keys[(i + 1) % len(keys)]
        page.button(250, TOP, 100, 18, '< ' + records.PLAYER_BOARDS[prev][1], self.url('lb.svml', b=prev))
        page.button(360, TOP, 100, 18, records.PLAYER_BOARDS[nxt][1] + ' >', self.url('lb.svml', b=nxt))
        rows = []
        for pos, account, name, value, s in board[p * ROWS_PER_PAGE:(p + 1) * ROWS_PER_PAGE]:
            rows.append([pos, (name, self.url('agent.svml', id=account)), s['rank'], value, '%d/%d' % (s['wins'], s['games'])])
        viewer = self.viewer(req)
        mine = next(([pos, name, s['rank'], value, '%d/%d' % (s['wins'], s['games'])]
                     for pos, account, name, value, s in board if account == viewer), None)
        page.grid(20, TOP + 28, [('#', 40, 'center'), ('Agent', 190, 'left'), ('Rank', 50, 'center'),
                                 (heading, 80, 'right'), ('W/G', 80, 'right')], rows, mine)
        self.pager(page, 'lb.svml', p, len(board) > (p + 1) * ROWS_PER_PAGE, b=key)
        return page.render()

    def find(self, req):
        page = Page('Find an Agent', 'find')
        page.raw('<FORM name="findform" action="%s" method="GET">'
                 '<TEXT name="fl" x="40" y="%d" textColor="%s">Online ID:</TEXT>'
                 '<INPUT type="text" name="name" x="140" y="%d" width="260" height="22" maxLength="16" input_title="Online ID"'
                 ' selectable="default" fillColor="#FF140505" lineColor="%s" highlightFillColor="#FF281010"'
                 ' highlightLineColor="%s" highlightTextColor="%s" textColor="%s"/>'
                 '<INPUT type="submit" name="go" value="Search" x="190" y="%d" width="100" height="24" fillColor="%s"'
                 ' lineColor="%s" textColor="%s" highlightFillColor="%s" highlightTextColor="%s"/>'
                 '</FORM>' % (esc(self.svo.uri('agent.svml')), TOP + 44, GOLD, TOP + 40, LINE_C, GOLD, WHITE, WHITE,
                              TOP + 80, BTN_FILL, LINE_C, WHITE, GOLD, BLACK))
        page.text(240, TOP + 130, 'Press X on the box to type a name.', color=GREY, align='center')
        return page.render()

    def agent(self, req):
        q = req.query
        account = None
        if q.get('id'):
            account = self.store.account_by_id(_int(q, 'id'))
        elif q.get('name'):
            account = self.store.find_account(self.server.game.app_ids[0], q.get('name', '').strip())
        if account is None:
            page = Page('Find an Agent', 'agent')
            page.text(240, TOP + 60, 'No agent called "%s".' % q.get('name', ''), align='center')
            page.button(190, TOP + 90, 100, 24, 'Search again', self.svo.uri('find.svml'), default=True)
            return page.render()
        s = stats.decode_player(account['stats'])
        page = Page(account['name'], 'agent')
        online = self.server.lobby.find_player(account_id=account['account_id'])
        seen = 'online now' if online else 'last seen %s' % ago(account['last_login']) if account['last_login'] else ''
        if seen:
            page.text(460, TOP + 4, seen, color=GREY, align='right')
        cells = self.store.clans_of(account['account_id'])
        if cells:
            page.button(300, TOP + 27, 160, 18, 'Cell: %s' % cells[0]['name'], self.url('cell.svml', id=cells[0]['clan_id']))
        if s is None:
            page.text(20, TOP + 34, 'No ranked games played yet.', color=GREY)
            return page.render()
        xp_board = self.records.board('xp')
        pos = next((p for p, a, n, v, r in xp_board if a == account['account_id']), None)
        page.text(20, TOP + 30, 'Rank %s   %d XP%s' % (s['rank'], s['xp'], '   #%d of %d' % (pos, len(xp_board)) if pos else ''),
                  color=GOLD)
        rows = [[stats.MODE_NAMES[m], s['played'][m], s['won'][m],
                 '%d%%' % (100 * s['won'][m] // s['played'][m]) if s['played'][m] else '-'] for m in stats.MODES]
        rows.append(['All', s['games'], s['wins'], '%d%%' % (100 * s['wins'] // s['games']) if s['games'] else '-'])
        page.grid(20, TOP + 52, [('Combat type', 140, 'left'), ('Played', 60, 'right'), ('Won', 60, 'right'),
                                 ('Win %', 60, 'right')], rows, visible=6)
        y = TOP + 52
        for label, value in (('Kills', s['kills']), ('Deaths', s['deaths']),
                             ('K/D', '%.2f' % (s['kills'] / max(1, s['deaths']))), ('Headshots', s['headshots']),
                             ('Revives', s['revives']), ('Objectives', s['objectives']), ('Suicides', s['suicides'])):
            page.text(350, y + 2, label, color=GREY)
            page.text(460, y + 2, value, align='right')
            y += 18
        self.native_button(page, 'VIEW_PLAYER', 'viewplayer_ui', accountName=account['name'])
        return page.render()

    def native_button(self, page, kind, ui, **attrs):
        """A button that opens the game's own profile screen for a player or cell (its LOBBY_PLUGIN
        hooks; the game looks the player or cell up through the lobby)."""
        name = page.button(330, 224, 130, 18, 'Game profile')
        page.raw('<LOBBY_PLUGIN type="%s" %s="%s"%s/>' % (kind, ui, name, Page.attrs(attrs)))

    # ---- cells

    def cells(self, req):
        q = req.query
        key = q.get('b') if q.get('b') in records.CELL_BOARDS else 'wins'
        p = max(0, _int(q, 'p'))
        title, heading = records.CELL_BOARDS[key]
        board = self.records.cell_board(key)
        page = Page(title, 'cells')
        keys = list(records.CELL_BOARDS)
        nxt = keys[(keys.index(key) + 1) % len(keys)]
        page.button(330, TOP, 130, 18, 'By %s >' % records.CELL_BOARDS[nxt][1].lower(), self.url('cells.svml', b=nxt))
        rows = []
        for pos, c in board[p * ROWS_PER_PAGE:(p + 1) * ROWS_PER_PAGE]:
            tag = (c['cell'] or {}).get('tag', '')
            rows.append([pos, (c['clan']['name'], self.url('cell.svml', id=c['clan']['clan_id'])), tag, c['members'],
                         '%d/%d' % (c['wins'], c['games']), c['xp']])
        page.grid(20, TOP + 28, [('#', 40, 'center'), ('Cell', 160, 'left'), ('Tag', 50, 'center'),
                                 ('Agents', 60, 'right'), ('W/G', 60, 'right'), ('XP', 70, 'right')], rows)
        self.pager(page, 'cells.svml', p, len(board) > (p + 1) * ROWS_PER_PAGE, b=key)
        return page.render()

    def cell(self, req):
        clan = self.store.clan(_int(req.query, 'id'))
        if clan is None:
            page = Page('Cell Standings', 'cell')
            page.text(240, TOP + 60, 'That cell no longer exists.', align='center')
            return page.render()
        info = stats.decode_cell(clan['stats']) or {}
        leader = self.store.account_by_id(clan['leader_id'])
        page = Page('%s%s' % (clan['name'], ' [%s]' % info['tag'] if info.get('tag') else ''), 'cell')
        page.text(460, TOP + 4, 'Leader: %s' % (leader['name'] if leader else '?'), color=GREY, align='right')
        motd = self.store.clan_messages(clan['clan_id'])
        page.textarea(20, TOP + 28, 440, 24, 'Message of the day: %s' % (motd[0]['text'] if motd else 'none'), 1,
                      scroll=False)
        results = self.store.cell_results(clan['clan_id'])
        y = TOP + 60
        for mode in records.TEAM_MODES:
            games, wins = results.get(mode, (0, 0))
            page.text(20, y, stats.MODE_NAMES[mode], color=GREY)
            page.text(200, y, '%d won / %d' % (wins or 0, games), align='right')
            y += 18
        members = []
        for m in self.store.clan_members(clan['clan_id']):
            s = stats.decode_player(m['stats'])
            members.append([(m['name'], self.url('agent.svml', id=m['account_id'])), s['rank'] if s else '-',
                            s['xp'] if s else 0])
        members.sort(key=lambda r: -r[2])
        page.grid(220, TOP + 60, [('Agent', 140, 'left'), ('Rank', 40, 'center'), ('XP', 60, 'right')], members,
                  visible=7)
        self.native_button(page, 'VIEW_CELL', 'viewcell_ui', cellID=clan['clan_id'])
        return page.render()

    # ---- missions

    def missions(self, req):
        q = req.query
        orders = {'rated': ('Top Rated Missions', 'rating DESC, m.plays DESC'),
                  'played': ('Most Played Missions', 'm.plays DESC, rating DESC'),
                  'new': ('Newest Missions', 'm.created DESC')}
        key = q.get('b') if q.get('b') in orders else 'rated'
        p = max(0, _int(q, 'p'))
        title, order = orders[key]
        rows = self.store.q('SELECT m.*, (SELECT AVG(vote) FROM mission_votes v WHERE v.mission_id = m.mission_id AND '
                            'v.vote > 0) AS rating FROM missions m WHERE m.hidden = 0 ORDER BY %s LIMIT ? OFFSET ?' % order,
                            (ROWS_PER_PAGE + 1, p * ROWS_PER_PAGE))
        page = Page(title, 'missions')
        keys = list(orders)
        nxt = keys[(keys.index(key) + 1) % len(keys)]
        page.button(330, TOP, 130, 18, orders[nxt][0].split()[0] + ' >', self.url('missions.svml', b=nxt))
        grid = [[(m['name'], self.url('mission.svml', id=m['mission_id'])), m['author'],
                 MODE_SHORT.get(m['combat_type'], '?'), '%.1f' % m['rating'] if m['rating'] else '-', m['plays']]
                for m in rows[:ROWS_PER_PAGE]]
        page.grid(20, TOP + 28, [('Mission', 160, 'left'), ('Author', 110, 'left'), ('Type', 50, 'center'),
                                 ('Rating', 60, 'right'), ('Plays', 60, 'right')], grid)
        self.pager(page, 'missions.svml', p, len(rows) > ROWS_PER_PAGE, b=key)
        return page.render()

    def mission(self, req):
        m = self.store.one('SELECT * FROM missions WHERE mission_id = ? AND hidden = 0', (_int(req.query, 'id'),))
        page = Page(m['name'] if m else 'Mission Charts', 'mission')
        if m is None:
            page.text(240, TOP + 60, 'That mission is gone.', align='center')
            return page.render()
        votes = self.store.one('SELECT COUNT(*) AS n, AVG(vote) AS avg FROM mission_votes WHERE mission_id = ? AND vote > 0',
                               (m['mission_id'],))
        lines = [('Author', m['author'] + (' (official)' if m['official'] else '')),
                 ('Combat type', MODE_LONG.get(m['combat_type'], '?')),
                 ('Rating', '%.1f from %d votes' % (votes['avg'], votes['n']) if votes['n'] else 'not rated yet'),
                 ('Played', '%d times' % m['plays']), ('Downloads', m['downloads']),
                 ('Average game', '%d minutes' % (m['play_time'] // m['plays']) if m['plays'] and m['play_time'] else '-')]
        if m['team_a_wins'] or m['team_b_wins']:
            lines.append(('Team wins', 'A %d, B %d' % (m['team_a_wins'], m['team_b_wins'])))
        y = TOP + 32
        for label, value in lines:
            page.text(30, y, label, color=GREY)
            page.text(200, y, value)
            y += 20
        page.text(240, y + 10, 'Host it from Mission List in the online menu.', color=GREY, align='center')
        return page.render()

    # ---- the message board

    def boards(self, req):
        page = Page('Message Board', 'board')
        for i, (key, label) in enumerate(BOARDS):
            n = self.store.board_thread_count(key)
            page.button(60, TOP + 34 + i * 40, 360, 30, '%s  (%d topic%s)' % (label, n, '' if n == 1 else 's'),
                        self.url('topics.svml', b=key), default=i == 0)
        return page.render()

    def board_name(self, key):
        return dict(BOARDS).get(key)

    def topics(self, req):
        q = req.query
        key = q.get('b') if self.board_name(q.get('b')) else BOARDS[0][0]
        p = max(0, _int(q, 'p'))
        page = Page(self.board_name(key), 'topics')
        page.button(330, TOP, 130, 18, 'New topic', self.url('post.svml', b=key))
        threads = self.store.board_threads(key, p * ROWS_PER_PAGE, ROWS_PER_PAGE + 1)
        rows = [[(t['subject'] or '(no subject)', self.url('topic.svml', id=t['post_id'])), t['author'], t['replies'],
                 ago(t['active'])] for t in threads[:ROWS_PER_PAGE]]
        page.grid(20, TOP + 28, [('Topic', 200, 'left'), ('By', 110, 'left'), ('Replies', 60, 'right'),
                                 ('Active', 70, 'right')], rows)
        self.pager(page, 'topics.svml', p, len(threads) > ROWS_PER_PAGE, b=key)
        return page.render()

    def topic(self, req):
        post = self.store.board_post(_int(req.query, 'id'))
        if post is None or post['reply_to']:
            page = Page('Message Board', 'topic')
            page.text(240, TOP + 60, 'That topic was removed.', align='center')
            return page.render()
        page = Page(post['subject'] or '(no subject)', 'topic')
        posts = [post] + self.store.board_replies(post['post_id'])
        text = '\n\n'.join('%s, %s:\n%s' % (r['author'], ago(r['created']), r['body']) for r in posts)
        page.textarea(20, TOP + 28, 440, 152, text, 9)
        page.button(20, 224, 120, 18, 'Reply', self.url('post.svml', b=post['board'], reply=post['post_id']), default=True)
        page.button(340, 224, 120, 18, 'Back to topics', self.url('topics.svml', b=post['board']))
        return page.render()

    def compose(self, req):
        q = req.query
        key = q.get('b') if self.board_name(q.get('b')) else BOARDS[0][0]
        reply = _int(q, 'reply')
        parent = self.store.board_post(reply) if reply else None
        page = Page('Reply' if parent else 'New topic in %s' % self.board_name(key), 'post')
        if not self.viewer(req):
            page.text(240, TOP + 60, 'Sign in to Combat Ops online to post.', align='center')
            return page.render()
        fields = ['<FORM name="postform" action="%s" method="POST">' % esc(self.svo.uri('board/submit'))]
        y = TOP + 30
        if parent is None:
            fields.append('<TEXT name="ls" x="20" y="%d" textColor="%s">Subject</TEXT>'
                          '<INPUT type="text" name="subject" x="100" y="%d" width="360" height="22" maxLength="40"'
                          ' input_title="Subject" selectable="default" fillColor="#FF140505" lineColor="%s"'
                          ' highlightFillColor="#FF281010" highlightLineColor="%s" highlightTextColor="%s" textColor="%s"/>'
                          % (y + 4, GOLD, y, LINE_C, GOLD, WHITE, WHITE))
            y += 30
        else:
            fields.append('<TEXT name="lr" x="20" y="%d" textColor="%s">Re: %s</TEXT>' % (y + 4, GOLD,
                                                                                           esc(parent['subject'][:60])))
            y += 24
        fields.append('<TEXTAREA name="body" x="20" y="%d" width="440" height="%d" editable="true" maxLength="600"'
                      ' linesVisible="%d" lineSpacing="16" input_title="Message" is_short="false"%s fillColor="#FF140505"'
                      ' lineColor="%s" highlightLineColor="%s" textColor="%s" highlightTextColor="%s"></TEXTAREA>'
                      % (y, 222 - y - 6, (222 - y - 6) // 16, ' selectable="default"' if parent else '', LINE_C, GOLD,
                         WHITE, WHITE))
        fields.append('<INPUT type="hidden" name="b" value="%s"/><INPUT type="hidden" name="reply" value="%d"/>'
                      % (key, reply))
        fields.append('<INPUT type="submit" name="send" value="Post" x="190" y="222" width="100" height="20"'
                      ' fillColor="%s" lineColor="%s" textColor="%s" highlightFillColor="%s" highlightTextColor="%s"/>'
                      '</FORM>' % (BTN_FILL, LINE_C, WHITE, GOLD, BLACK))
        page.raw(''.join(fields))
        return page.render()

    def submit(self, req):
        q = req.query
        account = self.store.account_by_id(self.viewer(req)) if self.viewer(req) else None
        key = q.get('b') if self.board_name(q.get('b')) else BOARDS[0][0]
        reply = _int(q, 'reply')
        parent = self.store.board_post(reply) if reply else None
        body = ' '.join(q.get('body', '').split())[:600]
        subject = ' '.join(q.get('subject', '').split())[:40]
        now = int(time.time())
        problem = None
        if account is None or account['banned']:
            problem = 'Sign in to Combat Ops online to post.'
        elif not body:
            problem = 'The message was empty.'
        elif reply and parent is None:
            problem = 'That topic was removed.'
        elif self.store.recent_posts_by(account['account_id'], now - POST_INTERVAL):
            problem = 'Please wait a little before posting again.'
        elif self.store.recent_posts_by(account['account_id'], now - 86400) >= POSTS_PER_DAY:
            problem = 'That is enough posts for today.'
        page = Page('Message Board', 'posted')
        if problem:
            page.text(240, TOP + 60, problem, align='center')
            page.button(190, TOP + 90, 100, 24, 'Back', self.url('topics.svml', b=key), default=True)
            return page.render()
        if parent is not None:
            key, subject = parent['board'], parent['subject']
        post_id = self.store.add_board_post(key, account['account_id'], account['name'], subject or body[:40], body,
                                            parent['post_id'] if parent else 0)
        log.info('portal: %s posted %d in %s%s', account['name'], post_id, key, ' (reply to %d)' % reply if parent else '')
        page.text(240, TOP + 60, 'Posted.', align='center', color=GOLD)
        page.button(170, TOP + 90, 140, 24, 'Read the topic', self.url('topic.svml', id=parent['post_id'] if parent else post_id),
                    default=True)
        return page.render()


MODE_SHORT = {0: 'TDM', 1: 'RET', 2: 'SAB', 3: 'ROG', 4: 'DM'}
MODE_LONG = {0: 'Team Deathmatch', 1: 'Retrieval', 2: 'Sabotage', 3: 'Rogue Agent', 4: 'Deathmatch'}
