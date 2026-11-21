#!/usr/bin/env python3
"""List the files of CEV video assets that must not go into a game's pak files.

An EditorCEVPlayer video asset keeps more files in its folder than the game needs (docs/design/M4-asset-and-editor.md):

  <name>.cevvideo.cryasset   editor metadata. The engine's pak jobs already skip *.cryasset.
  <name>.webm (source copy)  the file as imported. It is also the video the descriptor plays until "Strip audio" /
                             a re-encode points the descriptor at another file. Only when the descriptor does NOT
                             play it, it is a pure source file and must not ship.
  <name>_<lang>.wav / .ogg   editor side exports. SDL_mixer plays its own copies under audio/sdlmixer/assets/cev/
                             (these ship with the audio folder), so the WAV / Ogg in the asset folder are not needed.

The script reads every <name>.cevvideo.cryasset under an Assets folder, reads the descriptor, and prints the files that
are listed in the asset but that the descriptor does not play.

    python tools/list_unshipped_cev_files.py <AssetsFolder> [--listfile out.txt] [--format rc|abs|rel]

  --listfile FILE   write the list to FILE instead of stdout
  --format rc       (default) paths relative to the Assets folder with backslashes, one per line: what the Resource
                    Compiler's /exclude_listfile= expects (see docs / README "Source file policy")
  --format rel      relative paths with forward slashes (e.g. for .gitignore / LFS / copy filters)
  --format abs      absolute paths

Exit status 0, also when nothing was found (the list is then empty).
"""
import argparse
import os
import sys
import xml.etree.ElementTree as ET


def read_cryasset(path):
    root = ET.parse(path).getroot()
    files = [f.get('path') for f in root.findall('./Files/File') if f.get('path')]
    details = {d.get('name'): (d.text or '') for d in root.findall('./Details/Detail')}
    return files, details


def read_descriptor(path):
    root = ET.parse(path).getroot()
    played = set()
    video = root.get('video')
    if video:
        played.add(video)
    for lang in root.findall('./Languages/Language'):
        if lang.get('file'):
            played.add(lang.get('file'))
    for variant in root.findall('./Variants/Variant'):
        if variant.get('file'):
            played.add(variant.get('file'))
    return played


def norm(p):
    return p.replace('\\', '/').lstrip('/')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('assets', help='the Assets folder of the project (the game root)')
    ap.add_argument('--listfile')
    ap.add_argument('--format', choices=['rc', 'rel', 'abs'], default='rc')
    args = ap.parse_args()

    assets = os.path.abspath(args.assets)
    if not os.path.isdir(assets):
        print('not a folder: ' + assets, file=sys.stderr)
        return 2

    result = []
    for folder, _dirs, names in os.walk(assets):
        for name in names:
            if not name.lower().endswith('.cevvideo.cryasset'):
                continue
            meta = os.path.join(folder, name)
            try:
                files, _details = read_cryasset(meta)
            except ET.ParseError as e:
                print('skipped (unreadable): %s: %s' % (meta, e), file=sys.stderr)
                continue
            if not files:
                continue
            # the File paths of a .cryasset are relative to the folder of the .cryasset itself
            meta_dir = norm(os.path.relpath(folder, assets))
            meta_dir = '' if meta_dir == '.' else meta_dir
            files = [norm(os.path.join(meta_dir, norm(f))) for f in files]
            descriptor_rel = norm(files[0])
            descriptor_abs = os.path.join(assets, descriptor_rel)
            descriptor_dir = os.path.dirname(descriptor_rel)
            try:
                played_names = read_descriptor(descriptor_abs)
            except (OSError, ET.ParseError) as e:
                print('skipped (descriptor unreadable): %s: %s' % (descriptor_abs, e), file=sys.stderr)
                continue
            # the descriptor names files relative to its own folder (or to the game root)
            played = set()
            for p in played_names:
                p = norm(p)
                played.add(norm(os.path.join(descriptor_dir, p)).lower())
                played.add(p.lower())
            for f in files:
                rel = norm(f)
                if rel.lower() == descriptor_rel.lower() or rel.lower() in played:
                    continue
                result.append(rel)

    result = sorted(set(result), key=str.lower)
    if args.format == 'rc':
        lines = [r.replace('/', '\\') for r in result]
    elif args.format == 'abs':
        lines = [os.path.join(assets, r).replace('/', os.sep) for r in result]
    else:
        lines = result

    text = ''.join(l + '\n' for l in lines)
    if args.listfile:
        with open(args.listfile, 'w', encoding='utf-8', newline='\n') as out:
            out.write(text)
        print('%d file(s) listed in %s' % (len(lines), args.listfile))
    else:
        sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
