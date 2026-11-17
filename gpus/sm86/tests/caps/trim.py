"""Trim a device capability capture to what `HostProfile::from_device` reads.

    python3 -I trim.py <capture> <out.json> <source> [report page]

The capture is a Vulkan Profiles JSON (`vulkaninfo --json`, or a vulkan.gpuinfo.org report's profile
download) or an Android vkjson (`adb shell cmd gpu vkjson`). The output keeps the capture's own layout
with only the extensions and the feature and property members listed below, plus a "source" line.

A vulkan.gpuinfo.org profile leaves out extensions newer than its schema (maximal reconvergence, FMA),
so for one the report's own page (displayreport.php?id=<id>) may be given as well: the extension
features it lists for the EXTENSIONS go in as the struct "VkPhysicalDeviceFeaturesFromReportPage".
"""
import json
import re
import sys

EXTENSIONS = {
    'VK_KHR_shader_maximal_reconvergence',
    'VK_KHR_shader_clock',
    'VK_KHR_shader_fma',
    'VK_EXT_shader_atomic_float',
    'VK_EXT_shader_image_atomic_int64',
    'VK_EXT_mesh_shader',
}
MEMBERS = {
    'deviceName', 'vendorID', 'deviceID', 'driverID',
    'subgroupSize', 'supportedStages', 'supportedOperations', 'subgroupSupportedStages', 'subgroupSupportedOperations',
    'subgroupSizeControl', 'computeFullSubgroups', 'minSubgroupSize', 'maxSubgroupSize',
    'requiredSubgroupSizeStages',
    'shaderMaximalReconvergence', 'shaderDemoteToHelperInvocation',
    'bufferDeviceAddress', 'shaderInt64',
    'shaderBufferInt64Atomics', 'shaderSharedInt64Atomics', 'shaderImageInt64Atomics',
    'shaderBufferFloat32AtomicAdd',
    'shaderDenormPreserveFloat32', 'shaderDenormFlushToZeroFloat32', 'shaderSignedZeroInfNanPreserveFloat32',
    'shaderFmaFloat32', 'shaderDeviceClock', 'shaderSubgroupClock',
    'maxComputeSharedMemorySize', 'maxDescriptorSetUpdateAfterBindSamplers',
    'maxDescriptorSetUpdateAfterBindSampledImages', 'maxPerStageDescriptorUpdateAfterBindStorageBuffers',
    'maxStorageBufferRange', 'maxComputeWorkGroupInvocations', 'maxComputeWorkGroupSize',
    'taskShader', 'meshShader', 'maxMeshOutputVertices', 'maxMeshOutputPrimitives', 'maxTaskPayloadSize',
    'maxTaskWorkGroupInvocations', 'maxMeshWorkGroupInvocations', 'maxMeshSharedMemorySize',
}


def prune(o):
    """Keep the MEMBERS anywhere in `o` with the objects on the way to them."""
    if not isinstance(o, dict):
        return None
    out = {}
    for k, v in o.items():
        if k in MEMBERS:
            out[k] = v
        else:
            p = prune(v)
            if p:
                out[k] = p
    return out


def page_features(path):
    """(extension, member, value) of each extension feature row of a gpuinfo report page."""
    text = open(path, encoding='utf-8').read()
    row = r"<td class='subkey'>(\w+)</td><td><div class='\w+'>(true|false)</div><td>(\w+)</td>"
    return [(e, m, v == 'true') for m, v, e in re.findall(row, text) if e in EXTENSIONS and m in MEMBERS]


src, dst, source = sys.argv[1:4]
d = json.load(open(src))
if 'capabilities' in d:
    caps = d['capabilities']
    dev = next(iter(caps.values()))
    keep = {
        'extensions': {k: v for k, v in dev.get('extensions', {}).items() if k in EXTENSIONS},
        'features': prune(dev.get('features', {})) or {},
        'properties': prune(dev.get('properties', {})) or {},
    }
    if len(sys.argv) > 4:
        page = {}
        for ext, member, value in page_features(sys.argv[4]):
            keep['extensions'].setdefault(ext, 1)
            page[member] = value
        if page:
            keep['features']['VkPhysicalDeviceFeaturesFromReportPage'] = page
    out = {'source': source, 'capabilities': {'device': keep}}
else:
    dev = d['devices'][0]
    keep = {k: p for k, p in ((k, prune(v)) for k, v in dev.items() if k not in ('extensions', 'formats', 'layers')) if p}
    keep['extensions'] = [e for e in dev.get('extensions', []) if e.get('extensionName') in EXTENSIONS]
    out = {'source': source, 'devices': [keep]}
json.dump(out, open(dst, 'w'), indent=1, sort_keys=True)
open(dst, 'a').write('\n')
