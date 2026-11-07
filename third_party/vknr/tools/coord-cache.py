"""Persist graph and temporal adapter pipelines after runtime use."""
from pathlib import Path
import os
p=Path(os.environ['TRP_VKNR_BUILD'])/'engine-src/src/core/nr_runtime.cpp'
t=p.read_text()
needle='        if (s.ctx.pipeline_cache) {\n            vkDestroyPipelineCache'
assert t.count(needle)==1
t=t.replace(needle,'        if (s.ctx.pipeline_cache) {\n            s.ctx.save_pipeline_cache();\n            vkDestroyPipelineCache')
p.write_text(t)
print('Prepared complete runtime pipeline-cache persistence')
