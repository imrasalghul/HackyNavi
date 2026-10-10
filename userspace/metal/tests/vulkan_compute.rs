//! Optional hardware test. Uses the installed Vulkan ICD, not HackyNavi's kext.
#![cfg(feature = "vulkan-test")]
use ash::{vk, Entry};
use hackynavi_metal_compiler::{compile, InputFormat, KernelDispatch, Request, ShaderStage};
use std::ffi::{CStr, CString};

#[test]
fn translated_air_executes_on_navi31() {
    unsafe {
        execute(false, false).expect("AIR add-one must execute correctly on a physical Navi31");
    }
}

#[test]
fn exact_thread_tail_dispatch_executes_on_navi31_without_surplus_writes() {
    unsafe {
        execute(true, false)
            .expect("exact-thread AIR dispatch must preserve the partial workgroup");
    }
}

#[test]
fn apple_generated_air_executes_on_navi31() {
    unsafe {
        execute(false, true).expect("Apple-generated AIR must execute correctly");
    }
}

#[test]
fn apple_generated_air_exact_thread_tail_executes_on_navi31() {
    unsafe {
        execute(true, true).expect("Apple AIR must preserve an exact-thread tail");
    }
}

unsafe fn execute(exact_threads: bool, apple_generated: bool) -> Result<(), String> {
    let check = |e| format!("Vulkan error: {e:?}");
    let loader = Entry::load().map_err(|e| e.to_string())?;
    let app = CString::new("HackyNavi AIR compiler semantics test").unwrap();
    let application = vk::ApplicationInfo::default()
        .application_name(&app)
        .api_version(vk::API_VERSION_1_2);
    let instance = loader
        .create_instance(
            &vk::InstanceCreateInfo::default().application_info(&application),
            None,
        )
        .map_err(check)?;
    let physicals = instance.enumerate_physical_devices().map_err(check)?;
    let physical = physicals
        .into_iter()
        .find(|&p| {
            let props = instance.get_physical_device_properties(p);
            props.vendor_id == 0x1002
                && props.device_id == 0x744c
                && props.device_type == vk::PhysicalDeviceType::DISCRETE_GPU
        })
        .ok_or("physical AMD Navi31 (1002:744c) not found; no software fallback is permitted")?;
    let props = instance.get_physical_device_properties(physical);
    println!(
        "AIR semantic test on {} ({:04x}:{:04x}), Vulkan {}.{}.{}; installed ICD, not HackyNavi",
        CStr::from_ptr(props.device_name.as_ptr()).to_string_lossy(),
        props.vendor_id,
        props.device_id,
        vk::api_version_major(props.api_version),
        vk::api_version_minor(props.api_version),
        vk::api_version_patch(props.api_version)
    );
    let family = instance
        .get_physical_device_queue_family_properties(physical)
        .iter()
        .position(|p| p.queue_count != 0 && p.queue_flags.contains(vk::QueueFlags::COMPUTE))
        .ok_or("no compute queue")? as u32;
    let priorities = [1.0];
    let queue_info = [vk::DeviceQueueCreateInfo::default()
        .queue_family_index(family)
        .queue_priorities(&priorities)];
    let device = instance
        .create_device(
            physical,
            &vk::DeviceCreateInfo::default().queue_create_infos(&queue_info),
            None,
        )
        .map_err(check)?;
    let queue = device.get_device_queue(family, 0);
    let memory_properties = instance.get_physical_device_memory_properties(physical);
    let count: usize = if exact_threads { 4099 } else { 4096 };
    let words = count + 64; // Detect unwanted writes by surplus invocations.
    let bytes = (words * 4) as u64;
    let mut buffers = Vec::new();
    for _ in 0..2 {
        let buffer = device
            .create_buffer(
                &vk::BufferCreateInfo::default()
                    .size(bytes)
                    .usage(vk::BufferUsageFlags::STORAGE_BUFFER)
                    .sharing_mode(vk::SharingMode::EXCLUSIVE),
                None,
            )
            .map_err(check)?;
        let requirements = device.get_buffer_memory_requirements(buffer);
        let memory_index = (0..memory_properties.memory_type_count)
            .find(|&i| {
                requirements.memory_type_bits & (1 << i) != 0
                    && memory_properties.memory_types[i as usize]
                        .property_flags
                        .contains(
                            vk::MemoryPropertyFlags::HOST_VISIBLE
                                | vk::MemoryPropertyFlags::HOST_COHERENT,
                        )
            })
            .ok_or("no host-visible coherent memory for compute test")?;
        let memory = device
            .allocate_memory(
                &vk::MemoryAllocateInfo::default()
                    .allocation_size(requirements.size)
                    .memory_type_index(memory_index),
                None,
            )
            .map_err(check)?;
        device
            .bind_buffer_memory(buffer, memory, 0)
            .map_err(check)?;
        let pointer = device
            .map_memory(memory, 0, bytes, vk::MemoryMapFlags::empty())
            .map_err(check)? as *mut u32;
        buffers.push((buffer, memory, pointer));
    }
    for i in 0..words {
        *buffers[0].2.add(i) = (i as u32).wrapping_mul(0x9e3779b9);
        *buffers[1].2.add(i) = 0xdeadbeef;
    }
    let shader = compile(&Request {
        input: if apple_generated {
            include_bytes!("fixtures/apple/add_one.air")
        } else {
            include_bytes!("fixtures/add_one.ll")
        },
        format: if apple_generated {
            InputFormat::AirBitcode
        } else {
            InputFormat::LlvmText
        },
        stage: ShaderStage::Compute,
        entry_point: "add_one",
        local_size: [64, 1, 1],
        dispatch: if exact_threads {
            KernelDispatch::safe_default()
        } else {
            KernelDispatch::Workgroups
        },
        raster_samples: None,
    })?;
    let bindings: Vec<_> = shader
        .reflection
        .bindings
        .iter()
        .map(|b| {
            let location = b.descriptor.as_ref().ok_or("buffer has no descriptor")?;
            if location.set != 0 || location.count != 1 {
                return Err("unexpected descriptor contract");
            }
            Ok(vk::DescriptorSetLayoutBinding::default()
                .binding(location.binding)
                .descriptor_count(1)
                .descriptor_type(vk::DescriptorType::STORAGE_BUFFER)
                .stage_flags(vk::ShaderStageFlags::COMPUTE))
        })
        .collect::<Result<_, _>>()?;
    if bindings.len() != 2 {
        return Err("expected input/output storage descriptors".into());
    }
    let layout = device
        .create_descriptor_set_layout(
            &vk::DescriptorSetLayoutCreateInfo::default().bindings(&bindings),
            None,
        )
        .map_err(check)?;
    let layouts = [layout];
    let ranges: Vec<_> = shader
        .reflection
        .kernel_dispatch
        .and_then(|d| d.push_constant_range())
        .into_iter()
        .map(|range| {
            vk::PushConstantRange::default()
                .offset(range.offset)
                .size(range.size)
                .stage_flags(vk::ShaderStageFlags::COMPUTE)
        })
        .collect();
    let pipeline_layout = device
        .create_pipeline_layout(
            &vk::PipelineLayoutCreateInfo::default()
                .set_layouts(&layouts)
                .push_constant_ranges(&ranges),
            None,
        )
        .map_err(check)?;
    let module = device
        .create_shader_module(
            &vk::ShaderModuleCreateInfo::default().code(&shader.spirv),
            None,
        )
        .map_err(check)?;
    let entry = CString::new(shader.spirv_entry_point.as_str()).unwrap();
    let stage = vk::PipelineShaderStageCreateInfo::default()
        .stage(vk::ShaderStageFlags::COMPUTE)
        .module(module)
        .name(&entry);
    let dispatch = shader
        .reflection
        .kernel_dispatch
        .ok_or("missing dispatch contract")?;
    let regions: Vec<([u32; 3], [u32; 3], Option<[u32; 12]>)> = if exact_threads {
        let plan = dispatch.plan([64, 1, 1], Some([count as u32, 1, 1]))?;
        plan.regions
            .iter()
            .map(|r| (r.local_size, r.group_count, Some(plan.push_constants(*r))))
            .collect()
    } else {
        vec![([64, 1, 1], [(count / 64) as u32, 1, 1], None)]
    };
    let mut pipelines = Vec::new();
    for (local, _, _) in &regions {
        let entries: Vec<_> = metal2vulkan::reflect::KERNEL_LOCAL_SIZE_SPEC_IDS
            .iter()
            .enumerate()
            .map(|(i, id)| {
                vk::SpecializationMapEntry::default()
                    .constant_id(*id)
                    .offset((i * 4) as u32)
                    .size(4)
            })
            .collect();
        let data: Vec<_> = local.iter().flat_map(|n| n.to_ne_bytes()).collect();
        let specialization = vk::SpecializationInfo::default()
            .map_entries(&entries)
            .data(&data);
        let specialized_stage = if exact_threads {
            stage.specialization_info(&specialization)
        } else {
            stage
        };
        pipelines.push(
            device
                .create_compute_pipelines(
                    vk::PipelineCache::null(),
                    &[vk::ComputePipelineCreateInfo::default()
                        .stage(specialized_stage)
                        .layout(pipeline_layout)],
                    None,
                )
                .map_err(|(_, e)| check(e))?[0],
        );
    }
    let sizes = [vk::DescriptorPoolSize::default()
        .ty(vk::DescriptorType::STORAGE_BUFFER)
        .descriptor_count(2)];
    let pool = device
        .create_descriptor_pool(
            &vk::DescriptorPoolCreateInfo::default()
                .max_sets(1)
                .pool_sizes(&sizes),
            None,
        )
        .map_err(check)?;
    let descriptor = device
        .allocate_descriptor_sets(
            &vk::DescriptorSetAllocateInfo::default()
                .descriptor_pool(pool)
                .set_layouts(&layouts),
        )
        .map_err(check)?[0];
    for (i, binding) in shader.reflection.bindings.iter().enumerate() {
        if binding.metal_index as usize != i {
            return Err("unexpected Metal buffer mapping".into());
        }
        let info = [vk::DescriptorBufferInfo::default()
            .buffer(buffers[i].0)
            .offset(0)
            .range(bytes)];
        device.update_descriptor_sets(
            &[vk::WriteDescriptorSet::default()
                .dst_set(descriptor)
                .dst_binding(binding.descriptor.as_ref().unwrap().binding)
                .descriptor_type(vk::DescriptorType::STORAGE_BUFFER)
                .buffer_info(&info)],
            &[],
        );
    }
    let command_pool = device
        .create_command_pool(
            &vk::CommandPoolCreateInfo::default().queue_family_index(family),
            None,
        )
        .map_err(check)?;
    let commands = device
        .allocate_command_buffers(
            &vk::CommandBufferAllocateInfo::default()
                .command_pool(command_pool)
                .level(vk::CommandBufferLevel::PRIMARY)
                .command_buffer_count(1),
        )
        .map_err(check)?;
    let command = commands[0];
    device
        .begin_command_buffer(
            command,
            &vk::CommandBufferBeginInfo::default()
                .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT),
        )
        .map_err(check)?;
    device.cmd_bind_descriptor_sets(
        command,
        vk::PipelineBindPoint::COMPUTE,
        pipeline_layout,
        0,
        &[descriptor],
        &[],
    );
    for (pipeline, (_, groups, payload)) in pipelines.iter().zip(&regions) {
        device.cmd_bind_pipeline(command, vk::PipelineBindPoint::COMPUTE, *pipeline);
        if let Some(payload) = payload {
            let data: Vec<_> = payload.iter().flat_map(|n| n.to_ne_bytes()).collect();
            device.cmd_push_constants(
                command,
                pipeline_layout,
                vk::ShaderStageFlags::COMPUTE,
                ranges[0].offset,
                &data,
            );
        }
        device.cmd_dispatch(command, groups[0], groups[1], groups[2]);
    }
    let barrier = [vk::MemoryBarrier::default()
        .src_access_mask(vk::AccessFlags::SHADER_WRITE)
        .dst_access_mask(vk::AccessFlags::HOST_READ)];
    device.cmd_pipeline_barrier(
        command,
        vk::PipelineStageFlags::COMPUTE_SHADER,
        vk::PipelineStageFlags::HOST,
        vk::DependencyFlags::empty(),
        &barrier,
        &[],
        &[],
    );
    device.end_command_buffer(command).map_err(check)?;
    let fence = device
        .create_fence(&vk::FenceCreateInfo::default(), None)
        .map_err(check)?;
    device
        .queue_submit(
            queue,
            &[vk::SubmitInfo::default().command_buffers(&commands)],
            fence,
        )
        .map_err(check)?;
    device
        .wait_for_fences(&[fence], true, 10_000_000_000)
        .map_err(check)?;
    let mut mismatch = None;
    for i in 0..words {
        let actual = *buffers[1].2.add(i);
        let expected = if i < count {
            (*buffers[0].2.add(i)).wrapping_add(1)
        } else {
            0xdeadbeef
        };
        if actual != expected {
            mismatch = Some(format!(
                "word {i}: expected {expected:08x}, got {actual:08x}"
            ));
            break;
        }
    }
    device.destroy_fence(fence, None);
    device.destroy_command_pool(command_pool, None);
    device.destroy_descriptor_pool(pool, None);
    for pipeline in pipelines {
        device.destroy_pipeline(pipeline, None);
    }
    device.destroy_shader_module(module, None);
    device.destroy_pipeline_layout(pipeline_layout, None);
    device.destroy_descriptor_set_layout(layout, None);
    for (buffer, memory, _) in buffers {
        device.unmap_memory(memory);
        device.destroy_buffer(buffer, None);
        device.free_memory(memory, None);
    }
    device.destroy_device(None);
    instance.destroy_instance(None);
    if let Some(error) = mismatch {
        return Err(error);
    }
    println!("Verified all {count} add-one output words and 64 untouched guard words on Navi31 (exact_threads={exact_threads}, apple_generated={apple_generated})");
    Ok(())
}
