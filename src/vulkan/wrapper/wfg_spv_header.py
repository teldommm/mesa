#!/usr/bin/env python3
# Compiles one WFG shader (wfg_shaders/, see LICENSE.wfg) with glslangValidator into a C
# header: `static const uint32_t <var>[]` and `<var>_size`, the form wrapper_wfg.c includes
# (as WinLite's wfg/bin2c.cmake wrote it).
#
#   wfg_spv_header.py GLSLANG INPUT OUTPUT VAR [-DNAME ...]
#
# Copyright © 2026 WinLite
# SPDX-License-Identifier: MIT

import os
import struct
import subprocess
import sys
import tempfile


def main():
    glslang, src, out, var = sys.argv[1:5]
    defines = sys.argv[5:]
    with tempfile.TemporaryDirectory() as tmp:
        spv = os.path.join(tmp, var + '.spv')
        cmd = [glslang, '-V', '--target-env', 'vulkan1.1', *defines, '-o', spv, src]
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode != 0:
            sys.stderr.write(r.stdout)
            sys.exit(r.returncode)
        with open(spv, 'rb') as f:
            data = f.read()
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    lines = []
    for i in range(0, len(words), 8):
        lines.append('    ' + ', '.join('0x%08x' % w for w in words[i:i + 8]) + ',')
    with open(out, 'w') as f:
        f.write('// Generated from %s by wfg_spv_header.py. Do not edit.\n' % os.path.basename(src))
        f.write('#pragma once\n#include <stdint.h>\n#include <stddef.h>\n\n')
        f.write('static const uint32_t %s[] = {\n%s\n};\n' % (var, '\n'.join(lines)))
        f.write('static const size_t %s_size = sizeof(%s);\n' % (var, var))


if __name__ == '__main__':
    main()
