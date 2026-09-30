"""Builds data/loader/stage_paths.txt: every path of the game's file list that belongs to a stage
(its path holds a stage code essNNNN_NN), one per line, lower case. The loader hashes them at
startup to tell which stage a mod changes. Input: REasy-parser's SF6_STM.list (point REASY_PARSER
at your checkout)."""
import os, re, sys
root = os.environ.get('REASY_PARSER', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'REasy-parser'))
src = os.path.join(root, 'resources', 'data', 'lists', 'SF6_STM.list')
out = os.path.join(os.path.dirname(__file__), '..', 'data', 'loader', 'stage_paths.txt')
pat = re.compile(r'ess\d{4}_\d{2}')
paths = sorted({l.strip().lower() for l in open(src, encoding='utf-8', errors='replace') if pat.search(l.lower())})
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(paths) + '\n')
codes = sorted({pat.search(p).group(0) for p in paths})
print(len(paths), 'paths,', len(codes), 'stage codes:', ' '.join(codes))
