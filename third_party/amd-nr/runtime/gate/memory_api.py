"""Modern CUDA memory ABIs traced by the diagnostic gate.

Legacy exports remain forwarders. GPA interception requires an exact pointer
match to one of these exports, preserving the driver's ABI and stream variant.
Signatures: NVIDIA CUDA Driver API, Memory Management / Stream Ordered Allocator.
"""

APIS = []

def add(name, args, ptr='ptr', count='0', width=1, stream='nullptr', allocate=False, free=False):
    APIS.append(dict(name=name, args=args, ptr=ptr, count=count, width=width,
                     stream=stream, allocate=allocate, free=free))

add('cuMemAlloc_v2', [('unsigned long long*', 'out'), ('size_t', 'bytes')],
    'out', 'bytes', allocate=True)
add('cuMemAllocManaged', [('unsigned long long*', 'out'), ('size_t', 'bytes'), ('unsigned', 'flags')],
    'out', 'bytes', allocate=True)
add('cuMemAllocPitch_v2', [('unsigned long long*', 'out'), ('size_t*', 'pitch'),
    ('size_t', 'width'), ('size_t', 'height'), ('unsigned', 'element')],
    'out', 'width * height', allocate=True)
for suffix in ['', '_ptsz']:
    add('cuMemAllocAsync' + suffix, [('unsigned long long*', 'out'), ('size_t', 'bytes'), ('void*', 'stream')],
        'out', 'bytes', stream='stream', allocate=True)
    add('cuMemAllocFromPoolAsync' + suffix, [('unsigned long long*', 'out'), ('size_t', 'bytes'),
        ('void*', 'pool'), ('void*', 'stream')], 'out', 'bytes', stream='stream', allocate=True)
    add('cuMemFreeAsync' + suffix, [('unsigned long long', 'ptr'), ('void*', 'stream')],
        stream='stream', free=True)
add('cuMemFree_v2', [('unsigned long long', 'ptr')], free=True)
for bits, value in [(8, 'unsigned char'), (16, 'unsigned short'), (32, 'unsigned')]:
    for dim in ['', '2']:
        stem = f'cuMemsetD{dim}D{bits}' if dim else f'cuMemsetD{bits}'
        for suffix, asynchronous in [('_v2', False), ('_v2_ptds', False), ('Async', True), ('Async_ptsz', True)]:
            args = [('unsigned long long', 'ptr')]
            if dim:
                args += [('size_t', 'pitch')]
            args += [(value, 'value')]
            args += [('size_t', 'width'), ('size_t', 'height')] if dim else [('size_t', 'count')]
            if asynchronous:
                args += [('void*', 'stream')]
            add(stem + suffix, args, count='width * height' if dim else 'count',
                width=bits // 8, stream='stream' if asynchronous else 'nullptr')

NAMES = {api['name'] for api in APIS}

def generate(target):
    lines = ['// Generated from memory_api.py; modern exact-export ABIs only.', '#pragma once', 'extern "C" {']
    for api in APIS:
        args = ', '.join(t + ' ' + n for t, n in api['args'])
        types = ', '.join(t for t, n in api['args'])
        values = ', '.join(n for t, n in api['args'])
        name = api['name']
        ptr = '0' if api['allocate'] else api['ptr']
        lines += [f'__declspec(dllexport) int {name}({args}) {{',
                  f'    using Fn = int (*)({types});',
                  f'    static auto fn = reinterpret_cast<Fn>(real("{name}"));',
                  '    if (!fn) return 801;',
                  '    if (!memory_trace::enabled()) return fn(' + values + ');',
                  '    const auto snapshot = memory_trace::observe();',
                  f'    const auto event = memory_trace::begin("{name}", {ptr}, {api["count"]}, {api["width"]}, {api["stream"]}, snapshot);',
                  f'    const int result = fn({values});']
        if name.startswith('cuMemset'):
            detail = 'pitch=%zu width=%zu height=%zu' if 'D2D' in name else 'elements=%zu'
            detailargs = 'pitch, width, height' if 'D2D' in name else 'count'
            lines.insert(len(lines)-1, f'    logf("C10 memory request id=%llu value=0x%x {detail}\\n", event, static_cast<unsigned>(value), {detailargs});')
        if api['allocate']:
            outptr = '(result == 0 && out) ? *out : 0'
            # For pitched memory only the successful returned pitch bounds the allocation.
            bytecount = '(pitch ? *pitch : 0) * height' if 'Pitch' in name else api['count']
            lines += [f'    const auto pointer = {outptr};',
                      f'    if (result == 0 && pointer) memory_trace::allocated(pointer, {bytecount}, snapshot);',
                      '    memory_trace::end(event, result, pointer);']
        else:
            if api['free']:
                lines += ['    if (result == 0) memory_trace::freed(ptr);']
            lines += [f'    memory_trace::end(event, result, {api["ptr"]});']
        lines += ['    return result;', '}']
    lines += ['}', 'namespace memory_trace {',
              'inline bool substitute(const char* symbol, void** pointer) {',
              '    if (!enabled() || !symbol || !pointer || !*pointer) return false;',
              '    if (std::strncmp(symbol, "cuMem", 5) != 0) return false;']
    for api in APIS:
        name = api['name']
        stem = name.split('_v2')[0].split('_ptsz')[0]
        lines += [f'    if ((std::strcmp(symbol, "{stem}") == 0 || std::strcmp(symbol, "{name}") == 0)',
                  f'        && *pointer == real("{name}")) {{',
                  f'        *pointer = reinterpret_cast<void*>(&{name});',
                  f'        logf("C10 memory lookup symbol=%s exactExport={name}\\n", symbol);',
                  '        return true;', '    }']
    lines += ['    logf("C10 memory lookup symbol=%s untraced implementation=%p\\n", symbol, *pointer);',
              '    return false;', '}', '}']
    with open(target, 'w', newline='\n') as output:
        output.write('\n'.join(lines) + '\n')

if __name__ == '__main__':
    import sys
    generate(sys.argv[1])
