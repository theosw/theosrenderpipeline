"""CPU-only compiled PTX bounds checks. Requires CUDA 12.9 ptxas; no GPU."""
from pathlib import Path
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parent))
import build_pascal_kernels as builder
import launch_bounds as bounds
import launch_metadata as metadata


class BoundsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory()
        root=Path(cls.temp.name)
        live='\n'.join(f'ld.volatile.global.u32 %r{i}, [%p+{i*4}];' for i in range(220))
        stores='\n'.join(f'st.volatile.global.u32 [%p+{i*4}], %r{i};' for i in range(220))
        cls.ptx='''.version 8.8
.target sm_61
.address_size 64
.visible .entry maximum() .maxntid 128,1,1 { .shared .align 16 .b8 cache[1024]; .reg .b32 %x; mov.u32 %x,0; st.shared.u32 [cache],%x; ret; }
.visible .entry required() .reqntid 32,4,1 { ret; }
.visible .entry registers(.param .u64 arg) {
.reg .b64 %p;
.reg .b32 %r<220>;
ld.param.u64 %p,[arg];
'''+live+'\n'+stores+'\nret;\n}\n'
        (root/'fixture.ptx').write_text(cls.ptx)
        ptxas=os.environ.get('TRP_TEST_PTXAS') or shutil.which('ptxas')
        if not ptxas:
            raise unittest.SkipTest('set TRP_TEST_PTXAS to a CUDA 12.9 ptxas (sm_61)')
        result=subprocess.run([ptxas,'-arch=sm_61','-O0',str(root/'fixture.ptx'),'-o',str(root/'fixture.cubin')],capture_output=True,text=True,
                              creationflags=getattr(subprocess,'BELOW_NORMAL_PRIORITY_CLASS',0))
        if result.returncode:
            raise RuntimeError(result.stdout+result.stderr)
        cls.cubin=(root/'fixture.cubin').read_bytes()
        cls.found=bounds.extract(cls.cubin,['maximum','required','registers'],61)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_compiled_ptx(self):
        self.assertEqual(self.found['maximum']['maxThreads'],128)
        self.assertEqual(self.found['maximum']['staticShared'],1024)
        self.assertEqual(self.found['maximum']['maxDynamicShared'],48128)
        self.assertEqual(self.found['required']['reqntid'],[32,4,1])
        r=self.found['registers']
        self.assertGreater(r['registers'],200)
        self.assertEqual(r['maxThreads'],256)
        self.assertEqual(r['maxDynamicShared'],49152)
        self.assertEqual(bounds.original_blocks(self.ptx),{'maximum':[128,1,1],'required':[32,4,1]})

    def test_missing_ffwd_refused(self):
        b=self.found['maximum']
        with self.assertRaisesRegex(RuntimeError,'supply a valid --launch'):
            bounds.validate('ffwd',b,[32,8,1],None)
        bounds.validate('ffwd',b,[32,8,1],dict(block=[32,4,1],shared=None))
        with self.assertRaises(RuntimeError):
            bounds.validate('ffwd',b,[32,8,1],dict(block=[32,8,1],shared=None))
        bounds.validate('unknown',b,None,None)
        with self.assertRaises(RuntimeError):
            bounds.validate('required',self.found['required'],[128,1,1],None)
        with self.assertRaises(RuntimeError):
            bounds.validate('shared',b,[128,1,1],dict(block=[128,1,1],shared=49153))

    def test_builder_refuses_without_metadata_opt_in(self):
        with tempfile.TemporaryDirectory() as work:
            folder=Path(work)/'bundle04'; folder.mkdir()
            original='.entry maximum() .maxntid 256,1,1 { ret; }'
            def assemble(cuda, program, *args, cwd):
                self.assertEqual(program,'ptxas')
                (cwd/args[-1]).write_bytes(self.cubin)
            with patch.object(builder.pascal_lowering,'lower',return_value=(original,{})), patch.object(builder.pascal_lowering,'replace_entries',return_value=(self.ptx,['maximum'])), patch.object(builder.ada,'run',side_effect=assemble):
                with self.assertRaisesRegex(RuntimeError,'supply a valid --launch'):
                    builder.build_bundle(None,4,original,'',Path(work),native=[self.ptx])

    def test_schema2_round_trip(self):
        records=bounds.add_records([],{4:{61:self.found,89:self.found}})
        pack=b'TRPNRK1\0'+struct.pack('<I',1)+bytes(500)
        result=metadata.append(pack,records)
        at=len(pack)
        self.assertEqual(result[at:at+16],b'TRPLCH1\0'+struct.pack('<II',2,3))
        at+=16
        decoded={}
        for _ in range(3):
            r=struct.unpack_from('<21I',result,at)
            name=result[at+84:at+84+r[2]].decode()
            decoded[name]=r
            at+=84+(r[2]+3)//4*4
        self.assertEqual(at,len(result))
        self.assertEqual(decoded['maximum'][4:7],(0,0,0))
        self.assertEqual(decoded['maximum'][11:16],(128,0,0,0,48128))
        self.assertEqual(decoded['required'][12:15],(32,4,1))
        self.assertEqual(decoded['registers'][11],256)

    def test_truncation_refused(self):
        for size in (0,5,63,struct.unpack_from('<Q',self.cubin,40)[0]+64*struct.unpack_from('<H',self.cubin,60)[0]-1):
            with self.assertRaises(RuntimeError):
                bounds.extract(self.cubin[:size],['maximum'],61)


if __name__=='__main__':
    unittest.main()
