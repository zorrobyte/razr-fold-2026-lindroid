// M2: turnip renders (clears) into a linear DRM-modifier image backed by
// exportable dma-buf memory, read back + verified, dma-buf fd exported.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { VkResult _r=(x); if(_r!=VK_SUCCESS){ fprintf(stderr,"FAIL %s = %d\n",#x,_r); exit(2);} } while(0)

static uint32_t findMem(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want){
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd,&mp);
    for(uint32_t i=0;i<mp.memoryTypeCount;i++)
        if((bits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&want)==want) return i;
    return UINT32_MAX;
}

int main(void){
    const uint32_t W=256,H=256; const VkFormat FMT=VK_FORMAT_R8G8B8A8_UNORM;

    VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app;
    VkInstance inst; CHECK(vkCreateInstance(&ici,0,&inst));

    uint32_t n=0; CHECK(vkEnumeratePhysicalDevices(inst,&n,0)); if(!n){fprintf(stderr,"no devices\n");return 2;}
    VkPhysicalDevice pds[8]; if(n>8)n=8; CHECK(vkEnumeratePhysicalDevices(inst,&n,pds));
    VkPhysicalDevice pd=pds[0];
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd,&props);
    printf("GPU: %s\n", props.deviceName);

    uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,0);
    VkQueueFamilyProperties qf[16]; if(qn>16)qn=16; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,qf);
    uint32_t qfam=UINT32_MAX;
    for(uint32_t i=0;i<qn;i++) if(qf[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){qfam=i;break;}
    if(qfam==UINT32_MAX){fprintf(stderr,"no gfx/compute queue\n");return 2;}

    const char* devext[]={"VK_KHR_external_memory","VK_KHR_external_memory_fd",
                          "VK_EXT_external_memory_dma_buf","VK_EXT_image_drm_format_modifier"};
    float prio=1.0f;
    VkDeviceQueueCreateInfo qci={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex=qfam; qci.queueCount=1; qci.pQueuePriorities=&prio;
    VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci; dci.enabledExtensionCount=4; dci.ppEnabledExtensionNames=devext;
    VkDevice dev; CHECK(vkCreateDevice(pd,&dci,0,&dev));
    VkQueue q; vkGetDeviceQueue(dev,qfam,0,&q);

    PFN_vkGetMemoryFdKHR pGetFd=(PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev,"vkGetMemoryFdKHR");
    PFN_vkGetImageDrmFormatModifierPropertiesEXT pGetMod=
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(dev,"vkGetImageDrmFormatModifierPropertiesEXT");
    if(!pGetFd||!pGetMod){fprintf(stderr,"missing ext fn\n");return 2;}

    // enumerate DRM modifiers for FMT
    VkDrmFormatModifierPropertiesListEXT ml={VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 fp2={VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2}; fp2.pNext=&ml;
    vkGetPhysicalDeviceFormatProperties2(pd,FMT,&fp2);
    uint32_t mc=ml.drmFormatModifierCount;
    VkDrmFormatModifierPropertiesEXT* mods=calloc(mc?mc:1,sizeof(*mods));
    ml.pDrmFormatModifierProperties=mods; vkGetPhysicalDeviceFormatProperties2(pd,FMT,&fp2);
    printf("R8G8B8A8_UNORM modifiers: %u\n", mc);
    int haveLinear=0; for(uint32_t i=0;i<mc;i++){
        printf("  mod=0x%016llx planes=%u\n",(unsigned long long)mods[i].drmFormatModifier,mods[i].drmFormatModifierPlaneCount);
        if(mods[i].drmFormatModifier==0ULL) haveLinear=1;
    }
    uint64_t chosen = haveLinear?0ULL:(mc?mods[0].drmFormatModifier:0ULL);
    printf("chosen modifier = 0x%016llx%s\n",(unsigned long long)chosen, haveLinear?" (LINEAR)":"");

    VkExternalMemoryImageCreateInfo emici={VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emici.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageDrmFormatModifierListCreateInfoEXT dml={VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
    dml.drmFormatModifierCount=1; dml.pDrmFormatModifiers=&chosen; emici.pNext=&dml;

    VkImageCreateInfo ic={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ic.pNext=&emici;
    ic.imageType=VK_IMAGE_TYPE_2D; ic.format=FMT; ic.extent.width=W; ic.extent.height=H; ic.extent.depth=1;
    ic.mipLevels=1; ic.arrayLayers=1; ic.samples=VK_SAMPLE_COUNT_1_BIT;
    ic.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ic.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ic.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img; CHECK(vkCreateImage(dev,&ic,0,&img));

    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev,img,&mr);
    uint32_t mt=findMem(pd,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(mt==UINT32_MAX) mt=findMem(pd,mr.memoryTypeBits,0);
    VkExportMemoryAllocateInfo ema={VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    ema.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryDedicatedAllocateInfo ded={VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; ded.image=img; ded.pNext=&ema;
    VkMemoryAllocateInfo mai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.pNext=&ded; mai.allocationSize=mr.size; mai.memoryTypeIndex=mt;
    VkDeviceMemory mem; CHECK(vkAllocateMemory(dev,&mai,0,&mem));
    CHECK(vkBindImageMemory(dev,img,mem,0));

    VkImageDrmFormatModifierPropertiesEXT gm={VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
    CHECK(pGetMod(dev,img,&gm));
    printf("bound image modifier = 0x%016llx\n",(unsigned long long)gm.drmFormatModifier);

    VkDeviceSize bytes=(VkDeviceSize)W*H*4;
    VkBufferCreateInfo bci={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size=bytes; bci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf; CHECK(vkCreateBuffer(dev,&bci,0,&buf));
    VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(dev,buf,&bmr);
    uint32_t bmt=findMem(pd,bmr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo bmai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; bmai.allocationSize=bmr.size; bmai.memoryTypeIndex=bmt;
    VkDeviceMemory bmem; CHECK(vkAllocateMemory(dev,&bmai,0,&bmem)); CHECK(vkBindBufferMemory(dev,buf,bmem,0));

    VkCommandPoolCreateInfo cpci={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpci.queueFamilyIndex=qfam;
    VkCommandPool cp; CHECK(vkCreateCommandPool(dev,&cpci,0,&cp));
    VkCommandBufferAllocateInfo cbai={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool=cp; cbai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount=1;
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev,&cbai,&cb));
    VkCommandBufferBeginInfo bi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer(cb,&bi));
    VkImageSubresourceRange rng={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageMemoryBarrier b1={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b1.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b1.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.srcAccessMask=0; b1.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b1.image=img; b1.subresourceRange=rng;
    b1.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b1.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,0,0,0,1,&b1);
    VkClearColorValue col; col.float32[0]=1.0f; col.float32[1]=0.0f; col.float32[2]=1.0f; col.float32[3]=1.0f;
    vkCmdClearColorImage(cb,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&col,1,&rng);
    VkImageMemoryBarrier b2=b1; b2.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b2.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,0,0,0,1,&b2);
    VkBufferImageCopy cpy; memset(&cpy,0,sizeof cpy);
    cpy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; cpy.imageSubresource.layerCount=1;
    cpy.imageExtent.width=W; cpy.imageExtent.height=H; cpy.imageExtent.depth=1;
    vkCmdCopyImageToBuffer(cb,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buf,1,&cpy);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cb;
    CHECK(vkQueueSubmit(q,1,&si,VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(q));

    void* map; CHECK(vkMapMemory(dev,bmem,0,bytes,0,&map));
    unsigned char* px=(unsigned char*)map;
    printf("pixel[0]      = %02x %02x %02x %02x (expect ff 00 ff ff)\n",px[0],px[1],px[2],px[3]);
    unsigned long midi=((unsigned long)(H/2)*W+(W/2))*4;
    printf("pixel[center] = %02x %02x %02x %02x\n",px[midi],px[midi+1],px[midi+2],px[midi+3]);
    unsigned long match=0,total=(unsigned long)W*H;
    for(unsigned long p=0;p<total;p++){unsigned char*c=px+p*4; if(c[0]==255&&c[1]==0&&c[2]==255&&c[3]==255)match++;}
    printf("matching pixels: %lu / %lu\n",match,total);
    vkUnmapMemory(dev,bmem);

    VkMemoryGetFdInfoKHR gfi={VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR}; gfi.memory=mem;
    gfi.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int dmafd=-1; VkResult er=pGetFd(dev,&gfi,&dmafd);
    if(er==VK_SUCCESS && dmafd>=0) printf("dma-buf export OK: fd=%d alloc=%llu bytes\n",dmafd,(unsigned long long)mr.size);
    else printf("dma-buf export FAILED: res=%d fd=%d\n",er,dmafd);

    int pass = (match==total) && (dmafd>=0);
    printf("RESULT: %s\n", pass?"PASS":"FAIL");
    return pass?0:1;
}
