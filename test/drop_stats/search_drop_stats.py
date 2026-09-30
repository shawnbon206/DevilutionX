"""Simulates game seeds starting at the current second and prints the ones that drop the wanted items.

Usually run through drops.ps1 search, which prepares the simulator. Directly:
    python test/drop_stats/search_drop_stats.py --bin ~/drop-stats/search-bin --minutes 5 --type ring --prefix Gold --suffix Life

The wishlist options are the same as for query_drop_stats.py; a prefix or suffix can carry a minimum for the
first number it shows, e.g. --prefix Obsidian:38 Gold:28. Ctrl+C stops early and still prints the summary.
"""

import argparse
import csv
import datetime
import os
import shutil
import subprocess
import sys
import time

from query_drop_stats import DIFFICULTIES, creation_time

WORKERS = 20
BELOW_NORMAL_PRIORITY_CLASS = 0x4000
CREATE_NO_WINDOW = 0x08000000


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bin', required=True, help='folder with drop_stats_test.exe and its DLLs')
    parser.add_argument('--out', help='folder for the workers\' files (default: a new folder next to --bin)')
    parser.add_argument('--minutes', type=float, default=5, help='how long to search')
    parser.add_argument('--start', type=int, help='first seed (default: the current second)')
    parser.add_argument('--type', nargs='+', default=[], help='item types: ring amulet sword axe mace bow staff helm shield light_armor medium_armor heavy_armor')
    parser.add_argument('--base', nargs='+', default=[], help='base items, e.g. Maul "Great Axe"')
    parser.add_argument('--prefix', nargs='+', default=[], help='wanted prefixes; add :N for a minimum, e.g. Obsidian:38 means resist all at least 38')
    parser.add_argument('--suffix', nargs='+', default=[], help='wanted suffixes; add :N for a minimum, e.g. life:28')
    parser.add_argument('--unique', nargs='+', default=[], help='wanted unique items')
    parser.add_argument('--either', action='store_true', help='prefix OR suffix on the lists instead of both')
    parser.add_argument('--difficulty', type=int, choices=[0, 1, 2], help='0 Normal, 1 Nightmare, 2 Hell (default: all)')
    parser.add_argument('--min-roll', type=int, help='each wanted prefix and suffix must have rolled at least this far up its range, 0-100; 80 means the top fifth')
    parser.add_argument('--seeds', type=int, default=20, help='how many of the best seeds to list at the end')
    parser.add_argument('--keep', action='store_true', help='keep the workers\' files (all simulated items) instead of deleting them')
    args = parser.parse_args()
    if not (args.type or args.base or args.prefix or args.suffix or args.unique):
        parser.error('give at least one of --type, --base, --prefix, --suffix, --unique')
    return args


def split_minimum(spec):
    """"Obsidian:38" -> ("Obsidian", 38): the first number the affix shows in game must be at least 38."""
    name, sep, minimum = spec.rpartition(':')
    if sep and minimum.isdigit():
        return name, int(minimum)
    return spec, None


def check_names(args):
    """Stops on a name the game doesn't have, before any time is spent simulating."""
    known = {}
    with open(os.path.join(args.bin, 'names.csv'), encoding='utf-8', newline='') as f:
        for row in csv.DictReader(f):
            known.setdefault(row['kind'], set()).add(row['name'].lower())
    known['type'] = {'ring', 'amulet', 'sword', 'axe', 'mace', 'bow', 'staff', 'helm', 'shield', 'light_armor', 'medium_armor', 'heavy_armor'}
    for kind, names in (('type', args.type), ('base', args.base), ('prefix', args.prefix), ('suffix', args.suffix), ('unique', args.unique)):
        for name in names:
            name = split_minimum(name)[0].lower()
            if kind == 'suffix' and name.startswith('of '):
                name = name[3:]
            if name not in known[kind]:
                close = sorted(n for n in known[kind] if n[:3] == name[:3])[:8]
                sys.exit(f'the game has no {kind} called "{name}"' + (f'; similar: {", ".join(close)}' if close else ''))


class Wishlist:
    def __init__(self, args):
        lower = lambda names: [n.lower() for n in names]
        self.types = set(lower(args.type))
        self.bases = set(lower(args.base))
        self.prefixes = dict(split_minimum(p) for p in lower(args.prefix))
        self.suffixes = {}
        for spec in lower(args.suffix):
            name, minimum = split_minimum(spec)
            self.suffixes[name[3:] if name.startswith('of ') else name] = minimum
        self.uniques = set(lower(args.unique))
        self.either = args.either
        self.difficulty = args.difficulty
        self.min_roll = args.min_roll

    def matches(self, row):
        if self.difficulty is not None and int(row['difficulty']) != self.difficulty:
            return False
        if self.types and row['item_type'] not in self.types:
            return False
        if self.bases and row['base_item'].lower() not in self.bases:
            return False
        if self.uniques and row['unique_name'].lower() not in self.uniques:
            return False
        has_prefix = self.affix_ok(self.prefixes, row['prefix'], row['prefix_value']) and self.roll_ok(row['prefix_roll'])
        has_suffix = self.affix_ok(self.suffixes, row['suffix'], row['suffix_value']) and self.roll_ok(row['suffix_roll'])
        if self.prefixes and self.suffixes:
            if not ((has_prefix or has_suffix) if self.either else (has_prefix and has_suffix)):
                return False
        elif self.prefixes and not has_prefix:
            return False
        elif self.suffixes and not has_suffix:
            return False
        return True

    def roll_ok(self, roll):
        return self.min_roll is None or (roll != '' and int(roll) >= self.min_roll)

    @staticmethod
    def affix_ok(wanted, name, value):
        name = name.lower()
        if name not in wanted:
            return False
        minimum = wanted[name]
        return minimum is None or (value != '' and int(value) >= minimum)


class Worker:
    def __init__(self, index, args, out_dir, first_seed, stop_at):
        self.index = index
        self.args = args
        self.out_dir = out_dir
        self.first_seed = first_seed
        self.stop_at = stop_at
        self.restarts = 0
        self.games_offset = 0
        self.items_offset = 0
        self.process = None
        self.log_path = None
        self.start()

    def start(self):
        env = dict(os.environ,
                   DROPSTATS_OUT_DIR=self.out_dir, DROPSTATS_WORKER=str(self.index),
                   DROPSTATS_FIRST_SEED=str(self.first_seed + self.index), DROPSTATS_SEED_STEP=str(WORKERS),
                   DROPSTATS_SEED_COUNT='100000000', DROPSTATS_STOP_AT=str(self.stop_at))
        self.log_path = os.path.join(self.out_dir, f'worker_{self.index}_{self.restarts}.log')
        with open(self.log_path, 'wb') as log:
            self.process = subprocess.Popen([os.path.join(self.args.bin, 'drop_stats_test.exe'), '--gtest_filter=DropStats.Record'],
                                            cwd=self.args.bin, env=env, stdout=log, stderr=subprocess.STDOUT,
                                            creationflags=BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW)

    def check(self):
        """Returns False once the worker is done; restarts it if it stopped early (a level that hangs)."""
        if self.process.poll() is None:
            return True
        with open(self.log_path, encoding='utf-8', errors='replace') as log:
            if any(line.startswith(f'worker {self.index} finished') for line in log):
                return False
        if self.restarts >= 20:
            print(f'worker {self.index} keeps stopping early, giving up on it; see {self.log_path}')
            return False
        self.restarts += 1
        self.start()
        return True


def complete_lines(path, offset):
    if not os.path.exists(path):
        return
    with open(path, 'rb') as f:
        f.seek(offset)
        for raw in f:
            if not raw.endswith(b'\n'):
                break
            offset += len(raw)
            yield raw.decode('utf-8'), offset


def read_new(worker, games, hits, wishlist):
    """Reads the seeds a worker finished since the last call. Returns how many seeds that was."""
    games_path = os.path.join(worker.out_dir, f'games_{worker.index}.csv')
    items_path = os.path.join(worker.out_dir, f'items_{worker.index}.csv')
    items_end = worker.items_offset
    finished = []
    for line, end in complete_lines(games_path, worker.games_offset):
        row = next(csv.reader([line]))
        if row[0] == 'game_seed':
            worker.games_offset = end
            continue
        games[(int(row[0]), int(row[1]))] = row[4]
        if row[1] == '2':
            items_end = int(row[6])
            worker.games_offset = end
            finished.append(int(row[0]))
    for line, end in complete_lines(items_path, worker.items_offset):
        if end > items_end:
            break
        worker.items_offset = end
        row = next(csv.reader([line]))
        if row[0] == 'game_seed':
            continue
        values = dict(zip(ITEM_HEADER, row))
        if wishlist.matches(values):
            hits.setdefault((int(values['game_seed']), int(values['difficulty'])), []).append(values)
    return finished


ITEM_HEADER = [
    'game_seed', 'difficulty', 'dlvl', 'set_level', 'source_kind', 'source_name', 'source_index',
    'item_type', 'base_item', 'item_level', 'quality',
    'prefix', 'prefix_text', 'prefix_value', 'prefix_value2',
    'suffix', 'suffix_text', 'suffix_value', 'suffix_value2',
    'unique_name', 'spell', 'charges', 'min_dam', 'max_dam', 'ac', 'max_dur', 'req_str', 'req_mag', 'req_dex',
    'item_value', 'name', 'idx', 'iseed', 'create_info', 'prefix_roll', 'suffix_roll',
]


def describe(item):
    powers = ', '.join(t for t in (item['prefix_text'], item['suffix_text']) if t)
    where = item['set_level'] or f"dlvl {item['dlvl']}"
    return f"{item['name']} ({powers}) - {where}, {item['source_kind']} {item['source_name']}"


def print_game(seed, difficulty, items, hung):
    warning = f'  WARNING: the game hangs entering {hung}, stay out of it' if hung else ''
    print(f'  seed {seed} ({DIFFICULTIES[difficulty]}): {len(items)} matching{warning}')
    print(f'      created {creation_time(seed)}')
    for item in items:
        print(f'      {describe(item)}')


def main():
    args = parse_args()
    check_names(args)
    wishlist = Wishlist(args)
    first_seed = args.start if args.start is not None else int(time.time())
    stop_at = int(time.time() + args.minutes * 60)
    out_dir = args.out or os.path.join(os.path.dirname(os.path.normpath(args.bin)), 'search', datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    os.makedirs(out_dir, exist_ok=True)

    print(f'Searching from {creation_time(first_seed)} for {args.minutes:g} minutes on {WORKERS} workers; Ctrl+C stops early.\n')
    workers = [Worker(i, args, out_dir, first_seed, stop_at) for i in range(WORKERS)]
    games = {}
    hits = {}
    reported = set()
    seeds_done = 0
    last_progress = time.time()
    try:
        running = list(workers)
        while running:
            time.sleep(2)
            running = [w for w in running if w.check()]
            for worker in workers:
                seeds_done += len(read_new(worker, games, hits, wishlist))
            for key in sorted(set(hits) - reported):
                if key in games:
                    print_game(key[0], key[1], hits[key], games[key])
                    reported.add(key)
            if time.time() - last_progress >= 30:
                print(f'... {seeds_done:,} seeds searched, {len({k[0] for k in hits}):,} with a match')
                last_progress = time.time()
    except KeyboardInterrupt:
        print('\nStopping...')
    finally:
        for worker in workers:
            if worker.process.poll() is None:
                worker.process.kill()
        for worker in workers:
            worker.process.wait()
    for worker in workers:
        seeds_done += len(read_new(worker, games, hits, wishlist))

    last_seed = max((k[0] for k in games), default=first_seed)
    print(f'\nSearched {seeds_done:,} seeds, games created {creation_time(first_seed)} to {creation_time(last_seed)}.')
    for difficulty in range(3):
        if args.difficulty is not None and difficulty != args.difficulty:
            continue
        searched = sum(1 for k in games if k[1] == difficulty)
        matching = sum(1 for k in hits if k[1] == difficulty)
        odds = f'1 in {searched / matching:,.0f}' if matching else 'none'
        print(f'  {DIFFICULTIES[difficulty]:<10} {matching:,} of {searched:,} games have a match ({odds})')
    if hits:
        print(f'\nBest seeds (up to {args.seeds}):')
        best = sorted(hits, key=lambda k: (-len(hits[k]), k[0]))[:args.seeds]
        for key in best:
            print_game(key[0], key[1], hits[key], games.get(key, ''))
    if args.keep:
        print(f'\nWorker files are in {out_dir}; load_drop_stats.py can put them in a database.')
    else:
        shutil.rmtree(out_dir, ignore_errors=True)


if __name__ == '__main__':
    main()
