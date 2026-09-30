"""Loads the CSV output of run_drop_stats.ps1 into a SQLite database.

Safe to run while workers are still writing: it only loads seeds whose rows are complete, remembers how far
it got in each file, and picks up from there on the next run.

    python test/drop_stats/load_drop_stats.py ~/drop-stats-data

Text columns are stored once in the `strings` table and referenced by id; query the `items_v` view to get
them back as text (see query_drop_stats.py).
"""

import argparse
import csv
import glob
import os
import sqlite3
import sys
import time

TEXT_COLUMNS = [
    'set_level', 'source_kind', 'source_name', 'item_type', 'base_item', 'quality',
    'prefix', 'prefix_text', 'suffix', 'suffix_text', 'unique_name', 'spell', 'name',
]
INT_COLUMNS = [
    'game_seed', 'difficulty', 'dlvl', 'source_index', 'item_level',
    'prefix_value', 'prefix_value2', 'suffix_value', 'suffix_value2',
    'charges', 'min_dam', 'max_dam', 'ac', 'max_dur', 'req_str', 'req_mag', 'req_dex',
    'item_value', 'idx', 'iseed', 'create_info',
]
ITEM_COLUMNS = [
    'game_seed', 'difficulty', 'dlvl', 'set_level', 'source_kind', 'source_name', 'source_index',
    'item_type', 'base_item', 'item_level', 'quality',
    'prefix', 'prefix_text', 'prefix_value', 'prefix_value2',
    'suffix', 'suffix_text', 'suffix_value', 'suffix_value2',
    'unique_name', 'spell', 'charges', 'min_dam', 'max_dam', 'ac', 'max_dur', 'req_str', 'req_mag', 'req_dex',
    'item_value', 'name', 'idx', 'iseed', 'create_info',
]

SCHEMA = """
CREATE TABLE IF NOT EXISTS run_info (key TEXT PRIMARY KEY, value TEXT) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS load_state (worker TEXT PRIMARY KEY, games_offset INTEGER, items_offset INTEGER) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS strings (id INTEGER PRIMARY KEY, text TEXT NOT NULL UNIQUE);
CREATE INDEX IF NOT EXISTS strings_nocase ON strings (text COLLATE NOCASE);
CREATE TABLE IF NOT EXISTS hung_levels (game_seed INTEGER, dlvl INTEGER, set_level INTEGER, level TEXT, PRIMARY KEY (game_seed, dlvl, set_level)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS games (
    game_seed INTEGER, difficulty INTEGER, quests TEXT, set_levels TEXT, hung_levels TEXT, item_rows INTEGER,
    PRIMARY KEY (game_seed, difficulty)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS items (
    game_seed INTEGER, difficulty INTEGER, dlvl INTEGER, set_level INTEGER, source_kind INTEGER, source_name INTEGER,
    source_index INTEGER, item_type INTEGER, base_item INTEGER, item_level INTEGER, quality INTEGER,
    prefix INTEGER, prefix_text INTEGER, prefix_value INTEGER, prefix_value2 INTEGER,
    suffix INTEGER, suffix_text INTEGER, suffix_value INTEGER, suffix_value2 INTEGER,
    unique_name INTEGER, spell INTEGER, charges INTEGER, min_dam INTEGER, max_dam INTEGER, ac INTEGER, max_dur INTEGER,
    req_str INTEGER, req_mag INTEGER, req_dex INTEGER, item_value INTEGER, name INTEGER,
    idx INTEGER, iseed INTEGER, create_info INTEGER);
CREATE VIEW IF NOT EXISTS items_v AS SELECT
    i.rowid AS item_id, i.game_seed, i.difficulty, i.dlvl, sl.text AS set_level, sk.text AS source_kind, sn.text AS source_name,
    i.source_index, it.text AS item_type, bi.text AS base_item, i.item_level, q.text AS quality,
    p.text AS prefix, pt.text AS prefix_text, i.prefix_value, i.prefix_value2,
    s.text AS suffix, st.text AS suffix_text, i.suffix_value, i.suffix_value2,
    u.text AS unique_name, sp.text AS spell, i.charges, i.min_dam, i.max_dam, i.ac, i.max_dur,
    i.req_str, i.req_mag, i.req_dex, i.item_value, n.text AS name, i.idx, i.iseed, i.create_info
FROM items i
LEFT JOIN strings sl ON sl.id = i.set_level
LEFT JOIN strings sk ON sk.id = i.source_kind
LEFT JOIN strings sn ON sn.id = i.source_name
LEFT JOIN strings it ON it.id = i.item_type
LEFT JOIN strings bi ON bi.id = i.base_item
LEFT JOIN strings q ON q.id = i.quality
LEFT JOIN strings p ON p.id = i.prefix
LEFT JOIN strings pt ON pt.id = i.prefix_text
LEFT JOIN strings s ON s.id = i.suffix
LEFT JOIN strings st ON st.id = i.suffix_text
LEFT JOIN strings u ON u.id = i.unique_name
LEFT JOIN strings sp ON sp.id = i.spell
LEFT JOIN strings n ON n.id = i.name;
"""

INDEXES = [
    'CREATE INDEX IF NOT EXISTS items_affixes ON items (prefix, suffix)',
    'CREATE INDEX IF NOT EXISTS items_suffix ON items (suffix)',
    'CREATE INDEX IF NOT EXISTS items_unique ON items (unique_name)',
    'CREATE INDEX IF NOT EXISTS items_base ON items (base_item)',
    'CREATE INDEX IF NOT EXISTS items_type ON items (item_type)',
    'CREATE INDEX IF NOT EXISTS items_game ON items (game_seed, difficulty)',
]


class Strings:
    def __init__(self, db):
        self.db = db
        self.ids = {text: id for id, text in db.execute('SELECT id, text FROM strings')}

    def id(self, text):
        if text == '':
            return None
        found = self.ids.get(text)
        if found is None:
            found = self.db.execute('INSERT INTO strings (text) VALUES (?)', (text,)).lastrowid
            self.ids[text] = found
        return found


def complete_lines(path, offset):
    """Yields (line, end offset) for every newline-terminated line after offset."""
    with open(path, 'rb') as f:
        f.seek(offset)
        for raw in f:
            if not raw.endswith(b'\n'):
                break
            offset += len(raw)
            yield raw.decode('utf-8'), offset


def parse_row(line):
    return next(csv.reader([line]))


def load_worker(db, strings, out_dir, worker):
    games_path = os.path.join(out_dir, f'games_{worker}.csv')
    items_path = os.path.join(out_dir, f'items_{worker}.csv')
    state = db.execute('SELECT games_offset, items_offset FROM load_state WHERE worker = ?', (worker,)).fetchone()
    games_offset, items_offset = state if state else (0, 0)

    # The games row for Hell is written last for each seed and carries the items file size after that seed.
    games_rows = []
    items_end = items_offset
    games_end = games_offset
    pending = []
    for line, end in complete_lines(games_path, games_offset):
        row = parse_row(line)
        if row[0] == 'game_seed':
            games_end = end
            continue
        pending.append(row)
        if row[1] == '2':
            games_rows.extend(pending)
            pending = []
            items_end = int(row[6])
            games_end = end
    if not games_rows:
        return 0

    db.executemany('INSERT OR REPLACE INTO games VALUES (?, ?, ?, ?, ?, ?)',
                   [(int(r[0]), int(r[1]), r[2], r[3], r[4], int(r[5])) for r in games_rows])

    placeholders = ', '.join('?' * len(ITEM_COLUMNS))
    insert = f'INSERT INTO items ({", ".join(ITEM_COLUMNS)}) VALUES ({placeholders})'
    batch = []
    loaded = 0
    for line, end in complete_lines(items_path, items_offset):
        if end > items_end:
            break
        row = parse_row(line)
        if row[0] == 'game_seed':
            continue
        values = dict(zip(ITEM_COLUMNS, row))
        batch.append(tuple(
            strings.id(values[c]) if c in TEXT_COLUMNS else (int(values[c]) if values[c] != '' else None)
            for c in ITEM_COLUMNS))
        if len(batch) >= 100000:
            db.executemany(insert, batch)
            loaded += len(batch)
            batch = []
    db.executemany(insert, batch)
    loaded += len(batch)

    db.execute('INSERT OR REPLACE INTO load_state VALUES (?, ?, ?)', (worker, games_end, items_end))
    return loaded


def load_hung_levels(db, out_dir):
    """Levels whose generation never finished; the real game most likely freezes when entering them."""
    for path in glob.glob(os.path.join(out_dir, 'hung_*.csv')):
        with open(path, encoding='utf-8', newline='') as f:
            rows = [(int(r[0]), int(r[1]), int(r[2]), r[3]) for r in csv.reader(f) if len(r) == 4 and r[0].isdigit()]
        db.executemany('INSERT OR REPLACE INTO hung_levels VALUES (?, ?, ?, ?)', rows)


def load_run_info(db, out_dir):
    info = {}
    for path in sorted(glob.glob(os.path.join(out_dir, 'run_info_*.txt'))):
        with open(path, encoding='utf-8') as f:
            for line in f:
                key, _, value = line.strip().partition('=')
                if key in ('worker', 'first_seed', 'seed_count'):
                    continue
                if key in info and info[key] != value:
                    sys.exit(f'{path}: {key}={value} differs from other workers ({info[key]}); these runs must not share a database')
                info[key] = value
    existing = dict(db.execute('SELECT key, value FROM run_info'))
    for key, value in info.items():
        if key in existing and existing[key] != value:
            sys.exit(f'run_info {key}={value} differs from the database ({existing[key]}); use a separate database')
    db.executemany('INSERT OR REPLACE INTO run_info VALUES (?, ?)', info.items())


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('out_dir', help='folder the workers write to')
    parser.add_argument('--db', help='database path (default: <out_dir>/drops.db)')
    args = parser.parse_args()

    db_path = args.db or os.path.join(args.out_dir, 'drops.db')
    db = sqlite3.connect(db_path)
    db.execute('PRAGMA journal_mode = WAL')
    db.execute('PRAGMA synchronous = NORMAL')
    db.executescript(SCHEMA)
    load_run_info(db, args.out_dir)
    load_hung_levels(db, args.out_dir)
    strings = Strings(db)

    started = time.time()
    total = 0
    for games_path in sorted(glob.glob(os.path.join(args.out_dir, 'games_*.csv'))):
        worker = os.path.basename(games_path)[len('games_'):-len('.csv')]
        loaded = load_worker(db, strings, args.out_dir, worker)
        db.commit()
        total += loaded
        print(f'worker {worker}: {loaded:,} item rows', flush=True)

    print('updating indexes...', flush=True)
    for statement in INDEXES:
        db.execute(statement)
    db.execute('ANALYZE')
    db.commit()
    games, = db.execute('SELECT COUNT(*) FROM games').fetchone()
    hung, = db.execute('SELECT COUNT(*) FROM hung_levels').fetchone()
    print(f'loaded {total:,} item rows in {time.time() - started:.0f} s; database has {games:,} games in {db_path}')
    print(f'{hung:,} levels left out because the game hangs generating them (table hung_levels, games.hung_levels)')


if __name__ == '__main__':
    main()
