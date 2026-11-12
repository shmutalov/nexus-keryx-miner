//! What the walk may use on the Vulkan device llama.cpp created. A borrowed `VkDevice` cannot be
//! asked which features were enabled on it, so this repeats ggml-vulkan's own rules
//! (`ggml_vk_get_device`, `get_device_architecture`, `ggml_vk_khr_cooperative_matrix_support` in
//! the pinned llama.cpp):
//!
//! - int8 cooperative matrices are enabled on AMD's own drivers only for RDNA3/RDNA4, which
//!   advertise the extension on every architecture;
//! - `VK_KHR_shader_integer_dot_product` is enabled only where packed signed 4x8-bit dot products
//!   are hardware-accelerated (not on RDNA1).
//!
//! A walk kernel that relied on a feature the device was created without would be invalid usage.

use ash::vk;
use ash::vk::Handle;
use std::ffi::CStr;

/// RDNA generations as ggml-vulkan classifies AMD devices.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AmdArch {
    Gcn,
    Rdna1,
    Rdna2,
    Rdna3,
    Rdna4,
    Other,
}

pub struct SharedDevice {
    pub name: String,
    pub vendor_id: u32,
    pub arch: Option<AmdArch>,
    /// llama.cpp enabled int8 cooperative matrices, so the matrix-core walk may run.
    pub matrix_cores: bool,
    /// llama.cpp enabled integer dot products, so the v_dot4 walk may run.
    pub integer_dot: bool,
}

const VENDOR_AMD: u32 = 0x1002;
const VENDOR_INTEL: u32 = 0x8086;
const VENDOR_QUALCOMM: u32 = 0x5143;

/// Inspect llama.cpp's device 0 (the inference GPU). The ggml Vulkan backend must be initialised.
pub fn shared_device() -> Result<SharedDevice, String> {
    let (mut instance, mut physical, mut device, mut family) =
        (std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut(), 0u32);
    // SAFETY: plain query into the initialised backend; the handles stay valid for the process.
    if !unsafe {
        llama_cpp_sys_2::ggml_backend_vk_keryx_raw_handles(0, &mut instance, &mut physical, &mut device, &mut family)
    } {
        return Err("llama.cpp has no Vulkan device with bufferDeviceAddress".into());
    }
    // SAFETY: ggml's live instance and physical device; only queries are issued.
    unsafe {
        let entry = ash::Entry::load().map_err(|e| format!("Vulkan loader not found: {e}"))?;
        let instance = ash::Instance::load(entry.static_fn(), vk::Instance::from_raw(instance as u64));
        Ok(classify(&instance, vk::PhysicalDevice::from_raw(physical as u64)))
    }
}

unsafe fn classify(instance: &ash::Instance, pd: vk::PhysicalDevice) -> SharedDevice {
    let exts: Vec<String> = instance
        .enumerate_device_extension_properties(pd)
        .unwrap_or_default()
        .iter()
        .filter_map(|e| e.extension_name_as_c_str().ok().map(|n| n.to_string_lossy().into_owned()))
        .collect();
    let has = |name: &CStr| exts.iter().any(|e| e.as_bytes() == name.to_bytes());
    let props = instance.get_physical_device_properties(pd);
    let name = CStr::from_ptr(props.device_name.as_ptr()).to_string_lossy().into_owned();

    let mut driver = vk::PhysicalDeviceDriverProperties::default();
    let mut dot = vk::PhysicalDeviceShaderIntegerDotProductProperties::default();
    let mut subgroup = vk::PhysicalDeviceSubgroupSizeControlProperties::default();
    let mut core = vk::PhysicalDeviceShaderCorePropertiesAMD::default();
    let amd_core = has(c"VK_AMD_shader_core_properties");
    {
        let mut p2 = vk::PhysicalDeviceProperties2::default()
            .push_next(&mut driver)
            .push_next(&mut dot)
            .push_next(&mut subgroup);
        if amd_core {
            p2 = p2.push_next(&mut core);
        }
        instance.get_physical_device_properties2(pd, &mut p2);
    }

    let arch = (props.vendor_id == VENDOR_AMD).then(|| {
        if !amd_core || !has(c"VK_KHR_shader_integer_dot_product") || !has(c"VK_EXT_subgroup_size_control") {
            AmdArch::Other
        } else if subgroup.min_subgroup_size == 64 && subgroup.max_subgroup_size == 64 {
            AmdArch::Gcn
        } else if subgroup.min_subgroup_size == 32 && subgroup.max_subgroup_size == 64 {
            if core.wavefronts_per_simd == 20 {
                AmdArch::Rdna1
            } else if has(c"VK_EXT_shader_float8") {
                AmdArch::Rdna4
            } else if dot.integer_dot_product4x8_bit_packed_mixed_signedness_accelerated == vk::TRUE {
                AmdArch::Rdna3
            } else {
                AmdArch::Rdna2
            }
        } else {
            AmdArch::Other
        }
    });

    let amd_driver = matches!(driver.driver_id, vk::DriverId::AMD_PROPRIETARY | vk::DriverId::AMD_OPEN_SOURCE);
    let coopmat_architecture = match props.vendor_id {
        VENDOR_AMD if amd_driver => matches!(arch, Some(AmdArch::Rdna3 | AmdArch::Rdna4)),
        // ggml limits Intel to Xe2 and Qualcomm to Adreno matrix-core parts; neither is admitted here.
        VENDOR_INTEL | VENDOR_QUALCOMM => false,
        _ => true,
    };
    let matrix_cores = coopmat_architecture
        && has(c"VK_KHR_cooperative_matrix")
        && std::env::var_os("GGML_VK_DISABLE_COOPMAT").is_none();
    let integer_dot = has(c"VK_KHR_shader_integer_dot_product")
        && std::env::var_os("GGML_VK_DISABLE_INTEGER_DOT_PRODUCT").is_none()
        && dot.integer_dot_product4x8_bit_packed_signed_accelerated == vk::TRUE;

    SharedDevice { name, vendor_id: props.vendor_id, arch, matrix_cores, integer_dot }
}
