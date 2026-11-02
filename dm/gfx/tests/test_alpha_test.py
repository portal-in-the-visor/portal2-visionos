"""NVN alpha rejection must prevent depth and stencil writes, including when color writes are off."""
import ast
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
source = ROOT / 'tools/dmsh_build.py'
nodes = [n for n in ast.parse(source.read_text()).body if isinstance(n, ast.FunctionDef) and n.name in ('fs_alpha_test', 'fs_uint')]
scope = {'re': re}
exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), scope)
SOURCE = '''#include <metal_stdlib>
using namespace metal;
struct main0_out {
    float4 color [[color(0)]];
};
fragment main0_out main0(float4 p [[position]]) {
    main0_out out = {};
    float values[4] = {0.0, 0.25, 0.5, 1.0};
    out.color = float4(1.0, 0.0, 0.0, values[uint(p.x)]);
    if (p.x < 2.0) return out;
    return out;
}
vertex float4 vtx(uint i [[vertex_id]]) {
    float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)};
    return float4(p[i],0.25,1);
}
kernel void read_planes(texture2d<uint,access::read> s [[texture(0)]], depth2d<float,access::read> d [[texture(1)]], device uint2 *o [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
    o[p.x] = uint2(s.read(p).r, as_type<uint>(d.read(p)));
}
'''

class AlphaTests(unittest.TestCase):
    def test_gpu_depth_stencil_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            msl = root / 'alpha.metal'; msl.write_text(scope['fs_alpha_test'](SOURCE))
            for sdk in ('macosx', 'iphoneos'):
                subprocess.run(['xcrun','-sdk',sdk,'metal','-c','-fmodules-cache-path='+str(root/sdk),str(msl),'-o',str(root/(sdk+'.air'))], check=True, capture_output=True)
            exe = root/'alpha-test'
            subprocess.run(['clang','-fobjc-arc','-framework','Metal','-framework','Foundation',str(ROOT/'tests/t_alpha_test.m'),'-o',str(exe)], check=True, capture_output=True)
            result = subprocess.run([str(exe),str(msl)], capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            # The unmodified shader must fail these literal plane expectations.
            msl.write_text(SOURCE)
            old = subprocess.run([str(exe),str(msl)], capture_output=True,text=True)
            self.assertNotEqual(old.returncode,0,'test must catch absent alpha rejection')
            print(result.stdout.strip())

if __name__ == '__main__': unittest.main()
