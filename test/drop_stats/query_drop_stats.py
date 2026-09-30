"""Searches the drop database for items with wanted affixes and reports how often and where they drop.

Rings or amulets whose prefix and suffix are both on the lists:
    python test/drop_stats/query_drop_stats.py ~/drop-stats-data/drops.db --type ring amulet --prefix "Dragon's" Gold Obsidian --suffix "the Zodiac" Perfection Life

Only one side given means only that side has to match; --either accepts an item that matches either list.
Names are case-insensitive and a leading "of " on suffixes is ignored.
"""

import argparse
import os
import sqlite3
import sys

DIFFICULTIES = ['Normal', 'Nightmare', 'Hell']


def string_ids(db, kind, names):
    """Looks up names in the strings table and stops with a message on a typo."""
    ids = []
    for name in names:
        if kind == 'suffix' and name.lower().startswith('of '):
            name = name[3:]
        rows = db.execute('SELECT id FROM strings WHERE text = ? COLLATE NOCASE', (name,)).fetchall()
        if not rows:
            sys.exit(f'no {kind} called "{name}" in the database (check the spelling)')
        ids.extend(row[0] for row in rows)
    return ids


def in_list(column, ids):
    return f'{column} IN ({", ".join(str(i) for i in ids)})'


def print_breakdown(db, title, expression, limit):
    total = db.execute('SELECT COUNT(*) FROM hits').fetchone()[0] or 1
    print(f'\n{title}')
    rows = db.execute(f"""SELECT {expression} AS k, COUNT(*) FROM hits h JOIN items_v v ON v.item_id = h.item_id
                          GROUP BY k ORDER BY COUNT(*) DESC LIMIT {limit}""")
    for key, count in rows:
        print(f'  {count:>10,}  {count / total:>6.1%}  {key}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('db', help='drops.db made by load_drop_stats.py')
    parser.add_argument('--type', nargs='+', default=[], help='item types: ring amulet sword axe mace bow staff helm shield light_armor medium_armor heavy_armor')
    parser.add_argument('--base', nargs='+', default=[], help='base items, e.g. Maul "Great Axe"')
    parser.add_argument('--prefix', nargs='+', default=[], help='wanted prefixes')
    parser.add_argument('--suffix', nargs='+', default=[], help='wanted suffixes')
    parser.add_argument('--unique', nargs='+', default=[], help='wanted unique items')
    parser.add_argument('--either', action='store_true', help='prefix OR suffix on the lists instead of both')
    parser.add_argument('--difficulty', type=int, choices=[0, 1, 2], help='0 Normal, 1 Nightmare, 2 Hell (default: all)')
    parser.add_argument('--min-prefix-value', type=int, help='minimum first number of the prefix, e.g. 150 for +150%% damage')
    parser.add_argument('--min-suffix-value', type=int, help='minimum first number of the suffix')
    parser.add_argument('--seeds', type=int, default=20, help='how many matching game seeds to list')
    parser.add_argument('--sql', action='store_true', help='print the SQL condition used')
    args = parser.parse_args()

    if not os.path.exists(args.db):
        sys.exit(f'{args.db} not found; run load_drop_stats.py first')
    db = sqlite3.connect(args.db)

    conditions = []
    if args.type:
        conditions.append(in_list('item_type', string_ids(db, 'item type', args.type)))
    if args.base:
        conditions.append(in_list('base_item', string_ids(db, 'base item', args.base)))
    affix = []
    if args.prefix:
        affix.append(in_list('prefix', string_ids(db, 'prefix', args.prefix)))
    if args.suffix:
        affix.append(in_list('suffix', string_ids(db, 'suffix', args.suffix)))
    if affix:
        conditions.append('(' + (' OR ' if args.either else ' AND ').join(affix) + ')')
    if args.unique:
        conditions.append(in_list('unique_name', string_ids(db, 'unique', args.unique)))
    if args.difficulty is not None:
        conditions.append(f'difficulty = {args.difficulty}')
    if args.min_prefix_value is not None:
        conditions.append(f'prefix_value >= {args.min_prefix_value}')
    if args.min_suffix_value is not None:
        conditions.append(f'suffix_value >= {args.min_suffix_value}')
    if not conditions:
        sys.exit('give at least one of --type, --base, --prefix, --suffix, --unique')
    where = ' AND '.join(conditions)
    if args.sql:
        print(f'WHERE {where}\n')

    db.execute(f'CREATE TEMP TABLE hits AS SELECT rowid AS item_id, game_seed, difficulty FROM items WHERE {where}')

    info = dict(db.execute('SELECT key, value FROM run_info'))
    print(f"Settings: {info.get('game_mode')} {info.get('version')}, multiplayer, full quests {info.get('full_quests')}, randomized quests {info.get('randomize_quests')}")

    print(f'\n{"difficulty":<12}{"games":>12}{"items":>12}{"per game":>11}  games with at least one')
    for difficulty, games in db.execute('SELECT difficulty, COUNT(*) FROM games GROUP BY difficulty ORDER BY difficulty').fetchall():
        if args.difficulty is not None and difficulty != args.difficulty:
            continue
        items, with_one = db.execute('SELECT COUNT(*), COUNT(DISTINCT game_seed) FROM hits WHERE difficulty = ?', (difficulty,)).fetchone()
        odds = f'1 in {games / with_one:,.0f}' if with_one else 'none'
        print(f'{DIFFICULTIES[difficulty]:<12}{games:>12,}{items:>12,}{items / games:>11.4f}  {with_one / games:.3%} ({odds})')

    print_breakdown(db, 'Items', "COALESCE(v.unique_name, TRIM(COALESCE(v.prefix, '') || ' ' || v.base_item || COALESCE(' of ' || v.suffix, '')))", 25)
    print_breakdown(db, 'Where', "COALESCE(v.set_level, 'dlvl ' || v.dlvl)", 30)
    print_breakdown(db, 'Source', "v.source_kind || ': ' || v.source_name", 25)

    print(f'\nGame seeds with the most matches (first {args.seeds})')
    seeds = db.execute(f"""SELECT game_seed, difficulty, COUNT(*) FROM hits GROUP BY game_seed, difficulty
                           ORDER BY COUNT(*) DESC, game_seed LIMIT {args.seeds}""").fetchall()
    for game_seed, difficulty, count in seeds:
        hung, = db.execute('SELECT hung_levels FROM games WHERE game_seed = ? AND difficulty = ?', (game_seed, difficulty)).fetchone()
        warning = f'  WARNING: the game hangs entering {hung}, stay out of it' if hung else ''
        print(f'  seed {game_seed} ({DIFFICULTIES[difficulty]}): {count}{warning}')
        rows = db.execute("""SELECT v.name, v.prefix_text, v.suffix_text, COALESCE(v.set_level, 'dlvl ' || v.dlvl), v.source_kind, v.source_name
                             FROM hits h JOIN items_v v ON v.item_id = h.item_id WHERE h.game_seed = ? AND h.difficulty = ?""", (game_seed, difficulty))
        for name, prefix_text, suffix_text, where_found, kind, source in rows:
            powers = ', '.join(t for t in (prefix_text, suffix_text) if t)
            print(f'      {name} ({powers}) - {where_found}, {kind} {source}')


if __name__ == '__main__':
    main()
