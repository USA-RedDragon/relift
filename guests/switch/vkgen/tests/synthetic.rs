//! The generator against a small synthetic registry in vk.xml's format:
//! layouts, translation classes, thunk and converter shapes, and the errors
//! it must raise.

use relift_switch_vkgen::model::Model;
use relift_switch_vkgen::registry::Registry;
use relift_switch_vkgen::select::Selection;
use relift_switch_vkgen::{Config, File, generate, sha256_hex};

const XML: &str = r#"<?xml version="1.0"?>
<registry>
<types>
  <type name="uint32_t"/><type name="uint64_t"/><type name="uint8_t"/><type name="char"/><type name="void"/><type name="float"/><type name="size_t"/>
  <type category="basetype">typedef <type>uint32_t</type> <name>VkFlags</name>;</type>
  <type category="basetype">typedef <type>uint64_t</type> <name>VkDeviceSize</name>;</type>
  <type category="bitmask">typedef <type>VkFlags</type> <name>VkFooFlags</name>;</type>
  <type category="handle"><type>VK_DEFINE_HANDLE</type>(<name>VkDevice</name>)</type>
  <type category="handle" parent="VkDevice"><type>VK_DEFINE_HANDLE</type>(<name>VkCommandBuffer</name>)</type>
  <type category="handle" parent="VkDevice"><type>VK_DEFINE_NON_DISPATCHABLE_HANDLE</type>(<name>VkFoo</name>)</type>
  <type name="VkStructureType" category="enum"/>
  <type name="VkResult" category="enum"/>
  <type category="struct" name="VkItem">
    <member><type>uint8_t</type> <name>tag</name></member>
    <member><type>VkDeviceSize</type> <name>size</name></member>
    <member><type>VkFoo</type> <name>foo</name></member>
  </type>
  <type category="struct" name="VkFooCreateInfo">
    <member values="VK_STRUCTURE_TYPE_FOO_CREATE_INFO"><type>VkStructureType</type> <name>sType</name></member>
    <member optional="true">const <type>void</type>* <name>pNext</name></member>
    <member optional="true"><type>VkFooFlags</type> <name>flags</name></member>
    <member><type>uint32_t</type> <name>itemCount</name></member>
    <member len="itemCount">const <type>VkItem</type>* <name>pItems</name></member>
    <member><type>uint32_t</type> <name>bufferCount</name></member>
    <member len="bufferCount">const <type>VkCommandBuffer</type>* <name>pBuffers</name></member>
    <member len="null-terminated">const <type>char</type>* <name>pName</name></member>
    <member><type>float</type> <name>weights</name>[<enum>VK_WEIGHT_COUNT</enum>]</member>
  </type>
  <type category="struct" name="VkFooExtra" structextends="VkFooCreateInfo">
    <member values="VK_STRUCTURE_TYPE_FOO_EXTRA"><type>VkStructureType</type> <name>sType</name></member>
    <member optional="true">const <type>void</type>* <name>pNext</name></member>
    <member><type>size_t</type> <name>bytes</name></member>
  </type>
  <type category="struct" name="VkFooInfoOut" returnedonly="true">
    <member values="VK_STRUCTURE_TYPE_FOO_INFO_OUT"><type>VkStructureType</type> <name>sType</name></member>
    <member optional="true"><type>void</type>* <name>pNext</name></member>
    <member><type>VkCommandBuffer</type> <name>owner</name></member>
    <member><type>uint32_t</type> <name>value</name></member>
  </type>
</types>
<enums name="API Constants" type="constants">
  <enum type="uint32_t" value="3" name="VK_WEIGHT_COUNT"/>
</enums>
<enums name="VkStructureType" type="enum">
  <enum value="0" name="VK_STRUCTURE_TYPE_FOO_CREATE_INFO"/>
  <enum value="1" name="VK_STRUCTURE_TYPE_FOO_INFO_OUT"/>
</enums>
<enums name="VkResult" type="enum">
  <enum value="0" name="VK_SUCCESS"/>
</enums>
<commands>
  <command>
    <proto><type>VkResult</type> <name>vkCreateFoo</name></proto>
    <param><type>VkDevice</type> <name>device</name></param>
    <param>const <type>VkFooCreateInfo</type>* <name>pCreateInfo</name></param>
    <param>const <type>VkAllocationCallbacks</type>* <name>pAllocator</name></param>
    <param><type>VkFoo</type>* <name>pFoo</name></param>
  </command>
  <command>
    <proto><type>void</type> <name>vkCmdFoo</name></proto>
    <param><type>VkCommandBuffer</type> <name>commandBuffer</name></param>
    <param><type>float</type> <name>width</name></param>
    <param><type>uint32_t</type> <name>a</name></param>
    <param><type>uint32_t</type> <name>b</name></param>
    <param><type>uint32_t</type> <name>c</name></param>
    <param><type>uint32_t</type> <name>d</name></param>
    <param><type>uint32_t</type> <name>e</name></param>
    <param><type>uint32_t</type> <name>f</name></param>
    <param><type>uint32_t</type> <name>g</name></param>
    <param><type>uint32_t</type> <name>h</name></param>
  </command>
  <command>
    <proto><type>VkResult</type> <name>vkGetFooInfo</name></proto>
    <param><type>VkDevice</type> <name>device</name></param>
    <param><type>uint32_t</type>* <name>pCount</name></param>
    <param len="pCount"><type>VkFooInfoOut</type>* <name>pInfos</name></param>
  </command>
  <command name="vkCreateFooKHR" alias="vkCreateFoo"/>
</commands>
<feature api="vulkan" name="VK_VERSION_1_0" number="1.0">
  <require>
    <type name="VkFooExtra"/>
    <type name="VkAllocationCallbacks"/>
    <command name="vkCreateFoo"/>
    <command name="vkCmdFoo"/>
    <command name="vkGetFooInfo"/>
    <enum extends="VkStructureType" extnumber="2" offset="0" name="VK_STRUCTURE_TYPE_FOO_EXTRA"/>
  </require>
</feature>
<feature api="vulkansc" name="VKSC_VERSION_1_0" number="1.0"><require><command name="vkCreateFoo"/></require></feature>
<extensions>
  <extension name="VK_KHR_foo" number="7" type="device" supported="vulkan">
    <require><command name="vkCreateFooKHR"/></require>
  </extension>
</extensions>
</registry>
"#;

const ALLOC: &str = r#"<type category="funcpointer"><proto><type>void</type>* <name>PFN_vkAllocationFunction</name></proto><param><type>void</type>* <name>pUserData</name></param></type>
<type category="struct" name="VkAllocationCallbacks"><member><type>void</type>* <name>pUserData</name></member><member><type>PFN_vkAllocationFunction</type> <name>pfnAllocation</name></member></type>"#;

fn xml() -> String {
    XML.replace("</types>", &format!("{ALLOC}\n</types>"))
}

fn config() -> Config {
    let mut c = Config::default();
    c.merge(
        "[api]\nfeatures = [\"VK_VERSION_1_0\"]\nextensions = [\"VK_KHR_foo\"]\n[thunks]\ndrop_types = [\"VkAllocationCallbacks\"]\n",
    )
    .unwrap();
    c
}

fn file<'a>(files: &'a [File], name: &str) -> &'a str {
    &files.iter().find(|f| f.name == name).unwrap().text
}

#[test]
fn layouts_follow_lp64() {
    let x = xml();
    let reg = Registry::parse(&x).unwrap();
    let cfg = config();
    let sel = Selection::new(&reg, &cfg).unwrap();
    let mut m = Model::new(&reg, &sel, &cfg).unwrap();
    let item = m.struct_layout("VkItem").unwrap();
    assert_eq!(item.offsets, [0, 8, 16]);
    assert_eq!((item.layout.size, item.layout.align), (24, 8));
    let ci = m.struct_layout("VkFooCreateInfo").unwrap();
    assert_eq!(ci.offsets, [0, 8, 16, 20, 24, 32, 40, 48, 56]);
    assert_eq!(ci.layout.size, 72);
    assert!(m.plain("VkItem"));
    assert!(!m.plain("VkFooCreateInfo"));
    assert!(
        !m.plain("VkFooExtra"),
        "a size_t member is always converted"
    );
    assert!(!m.plain("VkFooInfoOut"));
    assert_eq!(m.kind_of("VkCommandBuffer"), 1);
    assert_eq!(m.kind_of("VkFoo"), 0);
}

#[test]
fn thunks_and_converters_have_the_expected_shape() {
    let files = generate(&xml(), &config()).unwrap();
    let types = file(&files, "vkg_types.h");
    assert!(types.contains(
        "_Static_assert(sizeof(VkFooCreateInfo) == 72 && _Alignof(VkFooCreateInfo) == 8"
    ));
    assert!(types.contains("_Static_assert(offsetof(VkFooCreateInfo, weights) == 56"));
    assert!(types.contains("VK_STRUCTURE_TYPE_FOO_EXTRA = 1000001000,"));
    assert!(types.contains("typedef VkResult (VKAPI_PTR *PFN_vkCreateFooKHR)("));
    assert!(
        types.contains("typedef void * (VKAPI_PTR *PFN_vkAllocationFunction)(void* pUserData);")
    );
    let conv = file(&files, "vkg_convert.c");
    assert!(conv.contains("h->pItems = (const VkItem *)vkf_ptr(k, vkf_g64(k, g + 24));"));
    assert!(conv.contains("h->pBuffers = (const VkCommandBuffer *)vkf_unwrap_n(k, VKG_KIND_VkCommandBuffer, vkf_g64(k, g + 40), h->bufferCount);"));
    assert!(conv.contains("h->pNext = vkg_chain_in(k, vkf_g64(k, g + 8));"));
    assert!(conv.contains("case VK_STRUCTURE_TYPE_FOO_EXTRA:"));
    assert!(conv.contains("vkf_s64(k, g + 16, vkf_wrap(k, VKG_KIND_VkCommandBuffer, h->owner));"));
    let th = file(&files, "vkg_thunks.c");
    assert!(th.contains("const VkAllocationCallbacks * a2 = NULL;"));
    assert!(th.contains("float a1 = vkf_s(&k, 0);"));
    assert!(th.contains("uint32_t a2 = (uint32_t)(c->x[1]);"));
    assert!(th.contains("uint32_t a9 = (uint32_t)(vkf_stack(&k, 0));"));
    assert!(th.contains("vkg_aout_VkFooInfoOut(&k, c->x[2], a2, w2 < n2 ? w2 : n2);"));
    assert!(th.contains("{\"vkCreateFooKHR\", VKG_vkCreateFoo}"));
}

#[test]
fn refusals() {
    let mut cfg = config();
    cfg.registry_sha256 = "00".into();
    let e = generate(&xml(), &cfg).unwrap_err();
    assert!(e.0.contains("sha256"), "{e}");
    cfg.registry_sha256 = sha256_hex(xml().as_bytes());
    assert!(generate(&xml(), &cfg).is_ok());
    let bad = xml().replace("len=\"bufferCount\"", "len=\"bufferCount * 2\"");
    let e = generate(&bad, &config()).unwrap_err();
    assert!(e.0.contains("not a supported expression"), "{e}");
    let mut hand = config();
    hand.drop_types.clear();
    let e = generate(&xml(), &hand).unwrap_err();
    assert!(e.0.contains("VkAllocationCallbacks"), "{e}");
    let mut missing = config();
    missing.extensions.push("VK_KHR_nope".into());
    assert!(generate(&xml(), &missing).is_err());
    let mut void_check = config();
    void_check.check.insert("vkCmdFoo".into(), "f".into());
    let e = generate(&xml(), &void_check).unwrap_err();
    assert!(e.0.contains("check hook"), "{e}");
}

#[test]
fn a_check_hook_can_end_the_call() {
    let mut cfg = config();
    cfg.check.insert("vkCreateFoo".into(), "foo_check".into());
    let files = generate(&xml(), &cfg).unwrap();
    assert!(file(&files, "vkg.h").contains("VkResult foo_check(vkf_call *k);"));
    let th = file(&files, "vkg_thunks.c");
    let check = th.find("VkResult checked = foo_check(&k);").unwrap();
    assert!(th[check..].contains("if (checked < 0) {\n        c->x[0] = (uint32_t)checked;\n        return vkf_end(&k);\n    }"));
    assert!(check < th.find("VkResult r = fn(").unwrap());
}
