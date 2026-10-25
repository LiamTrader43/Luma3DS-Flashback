"""Pulls the recorder's code the tests need out of its sources, unchanged, into
one file the tests include (with their stubs for the 3DS's libraries in front
of it): flashback_internal.h and the recorder's files below, without their
#includes, and of their functions only those named below. (Everything else in
them, the state, the constants and the types, is kept.)

usage: python extract.py <rosalina source folder> <output .c>"""
import os
import re
import sys

SOURCE = sys.argv[1]
FILES = [os.path.join(SOURCE, '..', 'include', 'flashback_internal.h')] + \
        [os.path.join(SOURCE, name) for name in ['flashback.c', 'flashback_compress.c', 'flashback_audio.c',
                                                 'flashback_record.c', 'flashback_clip.c']]
FUNCTIONS = {
    # flashback.c
    'flashbackAudioFramesFor', 'flashbackLayout', 'flashbackSlotStride', 'flashbackVideoOffset',
    'flashbackApplyFormat',
    # flashback_compress.c (frames in the tests are compressed for real)
    'flashbackPackHash', 'flashbackUprightBand', 'flashbackPackPixels', 'flashbackCompress',
    'flashbackColumnsToRows',
    # flashback_audio.c
    'flashbackAudioSetCapture', 'flashbackAudioPause', 'flashbackAudioReset', 'flashbackAudioFramesIn',
    'flashbackAudioSamplesFor', 'flashbackAudioWanted', 'flashbackCounterNewer', 'flashbackAppendAudio',
    'flashbackAudioThreadMain',
    # flashback_record.c
    'flashbackWriteAudio', 'flashbackWritePending', 'flashbackWriterThreadMain', 'flashbackDrainWrites',
    'flashbackDrainCaptures', 'flashbackPackFrame', 'flashbackCaptureFrame', 'flashbackPackThreadMain',
    'flashbackFillMissed',
    # flashback_clip.c
    'flashbackPlanClip', 'flashbackPlanSlot', 'flashbackAudioOldest', 'flashbackAudioBase',
    'flashbackAudioSampleOf', 'flashbackBuildClipIndex',
}


def function_name(signature):
    """The name of the function a definition's first line starts."""
    for name in re.findall(r'(\w+)\s*\(', signature):
        if name != '__attribute__':
            return name
    raise SystemExit('no function name in: ' + signature)


found = set()


def extract(path):
    lines = open(path, encoding='utf-8').read().replace('\r\n', '\n').split('\n')
    out = []
    i = 0
    while i < len(lines):
        # A function: its first line at the left margin, any more lines of it
        # indented, then its body from '{' to '}' at the left margin.
        j = i
        while j + 1 < len(lines) and lines[j + 1].startswith((' ', '\t')):
            j += 1
        if (lines[i][:1].isalpha() or lines[i][:1] == '_') and j + 1 < len(lines) and lines[j + 1] == '{':
            k = j + 1
            while lines[k] != '}':
                k += 1
            name = function_name(lines[i])
            if name in FUNCTIONS:
                found.add(name)
                out.extend(lines[i:k + 1])
            i = k + 1
            continue
        if not lines[i].startswith(('#include', '#pragma once')):
            out.append(lines[i])
        i += 1
    return '\n'.join(out)


pieces = [extract(p) for p in FILES]
missing = FUNCTIONS - found
if missing:
    raise SystemExit('missing ' + ', '.join(sorted(missing)))
open(sys.argv[2], 'w', encoding='utf-8', newline='\n').write('\n\n'.join(pieces) + '\n')
print('extracted', len(found), 'functions from', len(FILES), 'files')
