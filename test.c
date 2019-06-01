#include "h.h"


static ULONG32  VmxAdjustControls (ULONG32 Ctl, ULONG32 Msr)
{
    LARGE_INTEGER MsrValue;

    MsrValue.QuadPart = __readmsr (Msr);
    Ctl &= MsrValue.HighPart;     /* bit == 0 in high word ==> must be zero */
    Ctl |= MsrValue.LowPart;      /* bit == 1 in low word  ==> must be one  */
    return Ctl;
}


SIZE_T get_segment_selector(IN SIZE_T segment_registers)
{
    SIZE_T segment_selector = segment_registers;

    //���ε�3λ������GDT�ĵ�ַ��ÿһ��8�ֽڶ��롣
    _bittestandreset64(&segment_selector, 0);
    _bittestandreset64(&segment_selector, 1);
    _bittestandreset64(&segment_selector, 2);

    return segment_selector;
}


SIZE_T Get_Segment_Base(IN SIZE_T Segment_Registers)
{
    SIZE_T Base = 0;
    SIZE_T BaseLow = 0;
    SIZE_T BaseMiddle = 0;
    SIZE_T BaseHigh = 0;
    PKGDTENTRY64 p = NULL;
    SIZE_T Segment_Selector;

    if (_bittest64(&Segment_Registers, 2) == 1)
    {
        //��LDT�С�
        return 0;
    }

    if (Segment_Registers > GetGdtLimit())
    {
        return 0;
    }

    //�������λ�ı�־��Ҳ���ǻ�ȡSegment Selector��
    Segment_Selector = get_segment_selector(Segment_Registers);

    p = (PKGDTENTRY64)((Segment_Selector) + (SIZE_T)(KeGetPcr()->GdtBase));

    Base = (p->Bytes.BaseHigh << 24) | (p->Bytes.BaseMiddle << 16) | (p->BaseLow);

    //if (p->Bits.DefaultBig && Base)
    //{
    //    //�����λΪ1.��F.
    //    Base += 0xffffffff00000000;
    //}

    //TSS���е����⡣�뿴��WINDBG��������鿴GDT����ʵ���ֵ������ÿ��CPU��PCR�л�ȡ��
    if (!(p->Bytes.Flags1 & 0x10)) {// this is a TSS or callgate etc, save the base high part    
        Base |= (*(PULONG64) ((PUCHAR) p + 8)) << 32;
    }

    return Base;
}


SIZE_T get_segments_access_right(SIZE_T segment_registers)
{
    SIZE_T access_right; 

    if (0 == segment_registers)
    {
        return 0x10000i64;//Ldtr�������Ϊ�η�����������д�˼����
    }

    access_right = get_access_rights(segment_registers);
    /*
    ����������ص��ǣ�Segment Descriptor�ĸ�DWORD������Ҳ�ų����DWORD�ĸ߰�λ��Base 31:24���͵Ͱ�λ��Base 23:16����
    */

    /*
    ��ʱʣ�����Чλ��ʮ��λ����һ��WORD�����ǵͰ�λҪ�Ƴ���
    ������16λ�л��м�λ��4λ��λ��Seg.Limit19:16������Ҫ�����
    �����д��㷨��
    �ɴ˿ɼ���segments_access_right����segments�г�ȥlimit��Base��ʣ�ಿ�֣����ʽ��WORD���ڡ�
    */
    access_right = (get_access_rights(segment_registers) >> 8) & 0xF0FF;
    
    return access_right;
}


PHYSICAL_ADDRESS NTAPI MmAllocateContiguousPages()
{
    PVOID PageVA;
    PHYSICAL_ADDRESS l1, l2, l3;

    l1.QuadPart = 0;
    l2.QuadPart = -1;
    l3.QuadPart = 0x200000;

    PageVA = MmAllocateContiguousMemorySpecifyCache (PAGE_SIZE, l1, l2, l3, MmCached);
    ASSERT (PageVA);
    RtlZeroMemory (PageVA, PAGE_SIZE);
    return MmGetPhysicalAddress (PageVA);
}


VOID SetVMCS (SIZE_T HostRsp, SIZE_T GuestRsp)
{
    SIZE_T           GdtBase = (SIZE_T)(KeGetPcr()->GdtBase);//r gdtr
    AMD64_DESCRIPTOR idtr    = {0};
    PKPCR            pcr     = KeGetPcr();
    PHYSICAL_ADDRESS IOBitmapAPA = MmAllocateContiguousPages();
    PHYSICAL_ADDRESS IOBitmapBPA = MmAllocateContiguousPages();
    PHYSICAL_ADDRESS MSRBitmapPA = MmAllocateContiguousPages();

    size_t x  = 0;
    unsigned char r = 0;

    VmxSecondaryProcessorBasedControls vm_procctl2_requested = { 0 };
    VmxSecondaryProcessorBasedControls vm_procctl2;
    VMX_CPU_BASED_CONTROLS vmCpuCtlRequested = {0};

    __sidt(&idtr.Limit);

    __vmx_vmwrite (VMCS_LINK_POINTER,      0xffffffff);
    __vmx_vmwrite (VMCS_LINK_POINTER_HIGH, 0xffffffff);

    __vmx_vmwrite (PIN_BASED_VM_EXEC_CONTROL, VmxAdjustControls(0, MSR_IA32_VMX_PINBASED_CTLS));

    // In order for our choice of supporting RDTSCP and XSAVE/RESTORES above to actually mean something, we have to request secondary controls.
    // We also want to activate the MSR bitmap in order to keep them from being caught.
    vmCpuCtlRequested.Fields.UseMSRBitmaps = 0;//���Ҫ�������߿�����ж�ػ�������
    vmCpuCtlRequested.Fields.ActivateSecondaryControl = TRUE;
    vmCpuCtlRequested.Fields.UseTSCOffseting = 0;
    vmCpuCtlRequested.Fields.RDTSCExiting = TRUE;//��RDTSCָ��Ĵ���WIN 10�ϱ���֧�֡�
    vmCpuCtlRequested.Fields.CR3LoadExiting = 0;// VPID caches must be invalidated on CR3 change
    x = VmxAdjustControls(vmCpuCtlRequested.All, 0x48E);
    __vmx_vmwrite(CPU_BASED_VM_EXEC_CONTROL, x);

    vm_procctl2_requested.fields.enable_ept = 0;
    vm_procctl2_requested.fields.descriptor_table_exiting = 0;
    vm_procctl2_requested.fields.enable_rdtscp = 1;  // for Win10
    vm_procctl2_requested.fields.enable_vpid = 0;
    vm_procctl2_requested.fields.enable_xsaves_xstors = 0;  // for Win10
    vm_procctl2.all = VmxAdjustControls(vm_procctl2_requested.all, 0x48B);
    __vmx_vmwrite(SECONDARY_VM_EXEC_CONTROL, vm_procctl2.all);

    __vmx_vmwrite (VM_EXIT_CONTROLS,  VmxAdjustControls(VM_EXIT_IA32E_MODE | VM_EXIT_ACK_INTR_ON_EXIT, MSR_IA32_VMX_EXIT_CTLS));
    __vmx_vmwrite (VM_ENTRY_CONTROLS, VmxAdjustControls(VM_ENTRY_IA32E_MODE, MSR_IA32_VMX_ENTRY_CTLS));

    __vmx_vmwrite (CR4_GUEST_HOST_MASK, X86_CR4_VMXE); 
    __vmx_vmwrite (CR4_READ_SHADOW,     __readcr4() & ~X86_CR4_VMXE);  
    __vmx_vmwrite (CR0_GUEST_HOST_MASK, X86_CR0_PG);
    __vmx_vmwrite (CR0_READ_SHADOW,     (__readcr4() & X86_CR0_PG) | X86_CR0_PG);

    __vmx_vmwrite (IO_BITMAP_A,      IOBitmapAPA.LowPart);
    __vmx_vmwrite (IO_BITMAP_A_HIGH, IOBitmapBPA.HighPart);
    __vmx_vmwrite (IO_BITMAP_B,      IOBitmapBPA.LowPart);
    __vmx_vmwrite (IO_BITMAP_B_HIGH, IOBitmapBPA.HighPart);
    __vmx_vmwrite (MSR_BITMAP,       MSRBitmapPA.LowPart);
    __vmx_vmwrite (MSR_BITMAP_HIGH,  MSRBitmapPA.HighPart);

    __vmx_vmwrite (EXCEPTION_BITMAP,  0);

    //////////////////////////////////////////////////////////////////////////////////////////////

    __vmx_vmwrite (GUEST_ES_SELECTOR,   RegGetEs());
    __vmx_vmwrite (GUEST_CS_SELECTOR,   RegGetCs());
    __vmx_vmwrite (GUEST_SS_SELECTOR,   RegGetSs());
    __vmx_vmwrite (GUEST_DS_SELECTOR,   RegGetDs());
    __vmx_vmwrite (GUEST_FS_SELECTOR,   RegGetFs());
    __vmx_vmwrite (GUEST_GS_SELECTOR,   RegGetGs());
    __vmx_vmwrite (GUEST_LDTR_SELECTOR, GetLdtr());
    __vmx_vmwrite (GUEST_TR_SELECTOR,   GetTrSelector());//Ӧ�ú�r tr�������Ľ��һ����

    __vmx_vmwrite (GUEST_ES_LIMIT, __segmentlimit(RegGetEs()));//__segmentlimit
    __vmx_vmwrite (GUEST_CS_LIMIT, __segmentlimit(RegGetCs()));
    __vmx_vmwrite (GUEST_SS_LIMIT, __segmentlimit(RegGetSs()));
    __vmx_vmwrite (GUEST_DS_LIMIT, __segmentlimit(RegGetDs()));
    __vmx_vmwrite (GUEST_FS_LIMIT, __segmentlimit(RegGetFs()));
    __vmx_vmwrite (GUEST_GS_LIMIT, __segmentlimit(RegGetGs()));
    __vmx_vmwrite (GUEST_TR_LIMIT, __segmentlimit(GetTrSelector()));
    __vmx_vmwrite (GUEST_IDTR_LIMIT,  idtr.Limit);//r idtl
    __vmx_vmwrite (GUEST_LDTR_LIMIT, __segmentlimit(GetLdtr()));
    __vmx_vmwrite (GUEST_GDTR_LIMIT,  GetGdtLimit()); //r gdtl

    __vmx_vmwrite (GUEST_ES_AR_BYTES,   get_segments_access_right(RegGetEs()));
    __vmx_vmwrite (GUEST_CS_AR_BYTES,   get_segments_access_right(RegGetCs()));
    __vmx_vmwrite (GUEST_SS_AR_BYTES,   get_segments_access_right(RegGetSs()));
    __vmx_vmwrite (GUEST_DS_AR_BYTES,   get_segments_access_right(RegGetDs()));
    __vmx_vmwrite (GUEST_FS_AR_BYTES,   get_segments_access_right(RegGetFs()));
    __vmx_vmwrite (GUEST_GS_AR_BYTES,   get_segments_access_right(RegGetGs()));
    __vmx_vmwrite (GUEST_LDTR_AR_BYTES, get_segments_access_right(GetLdtr()));
    __vmx_vmwrite (GUEST_TR_AR_BYTES,   get_segments_access_right(GetTrSelector()));

    __vmx_vmwrite (GUEST_FS_BASE,     __readmsr(MSR_FS_BASE));
    __vmx_vmwrite (GUEST_GS_BASE,     __readmsr(MSR_GS_BASE));
    __vmx_vmwrite (GUEST_TR_BASE,     (SIZE_T)KeGetPcr()->TssBase);//Get_Segment_Base(GetTrSelector())
    __vmx_vmwrite (GUEST_IDTR_BASE,   idtr.Base);//r idtr
    __vmx_vmwrite (GUEST_LDTR_BASE,   Get_Segment_Base(GetLdtr()));
    __vmx_vmwrite (GUEST_GDTR_BASE,   GdtBase);

    __vmx_vmwrite (GUEST_SYSENTER_CS,  __readmsr(MSR_IA32_SYSENTER_CS));
    __vmx_vmwrite (GUEST_SYSENTER_ESP, __readmsr(MSR_IA32_SYSENTER_ESP));
    __vmx_vmwrite (GUEST_SYSENTER_EIP, __readmsr(MSR_IA32_SYSENTER_EIP)); 

    __vmx_vmwrite (GUEST_CR0, __readcr0());
    __vmx_vmwrite (GUEST_CR3, __readcr3());
    __vmx_vmwrite (GUEST_CR4, __readcr4());

    __vmx_vmwrite (GUEST_DR7, 0x400);

    __vmx_vmwrite (GUEST_RSP,   GuestRsp);//GuestRsp (SIZE_T)CmSubvert
    __vmx_vmwrite (GUEST_RIP,   (SIZE_T)CmGuestEip);
    __vmx_vmwrite (GUEST_RFLAGS, __getcallerseflags());

    __vmx_vmwrite (GUEST_IA32_DEBUGCTL,      __readmsr(MSR_IA32_DEBUGCTL) & 0xffffffff);
    __vmx_vmwrite (GUEST_IA32_DEBUGCTL_HIGH, __readmsr (MSR_IA32_DEBUGCTL) >> 32);

    //////////////////////////////////////////////////////////////////////////////////////////////

    __vmx_vmwrite (HOST_CS_SELECTOR, get_segment_selector(RegGetCs()));
    __vmx_vmwrite (HOST_DS_SELECTOR, get_segment_selector(RegGetDs()));
    __vmx_vmwrite (HOST_ES_SELECTOR, get_segment_selector(RegGetEs()));
    __vmx_vmwrite (HOST_SS_SELECTOR, get_segment_selector(RegGetSs()));
    __vmx_vmwrite (HOST_FS_SELECTOR, get_segment_selector(RegGetFs()));
    __vmx_vmwrite (HOST_GS_SELECTOR, get_segment_selector(RegGetGs()));
    __vmx_vmwrite (HOST_TR_SELECTOR, get_segment_selector(GetTrSelector()));

    __vmx_vmwrite (HOST_FS_BASE,   __readmsr(MSR_FS_BASE));
    __vmx_vmwrite (HOST_GS_BASE,   __readmsr(MSR_GS_BASE));
    __vmx_vmwrite (HOST_TR_BASE,   (SIZE_T)KeGetPcr()->TssBase);
    __vmx_vmwrite (HOST_GDTR_BASE, GdtBase);
    __vmx_vmwrite (HOST_IDTR_BASE, idtr.Base);

    __vmx_vmwrite (HOST_IA32_SYSENTER_CS,  __readmsr(MSR_IA32_SYSENTER_CS));
    __vmx_vmwrite (HOST_IA32_SYSENTER_ESP, __readmsr(MSR_IA32_SYSENTER_ESP));
    __vmx_vmwrite (HOST_IA32_SYSENTER_EIP, __readmsr(MSR_IA32_SYSENTER_EIP));

    __vmx_vmwrite (HOST_CR0, __readcr0());
    __vmx_vmwrite (HOST_CR3, __readcr3());
    __vmx_vmwrite (HOST_CR4, __readcr4());

    __vmx_vmwrite (HOST_RSP, HostRsp);
    __vmx_vmwrite (HOST_RIP, (SIZE_T)VmxVmexitHandler);
}


VOID set_cr4()
{
    unsigned __int64 cr4 = 0;

    //����CR4��һ��λ��
    cr4 = __readcr4();

    //VMX-Enable Bit (bit 13 of CR4) �� Enables VMX operation when set
    cr4 = cr4 | 0x2000;//1>>13

    //��һ��˼·�ǣ�_bittestandset, _bittestandset64

    __writecr4((unsigned int)cr4);
}


NTSTATUS HvmSubvertCpu ()
{
    PHYSICAL_ADDRESS PhyAddr;
    unsigned char rc = 0;
    SIZE_T HostRsp;
    SIZE_T GuestRsp = (size_t)_AddressOfReturnAddress() + sizeof(void *);//���������е�RSP��ֵ��_ReturnAddress().
    PVOID Vmcs;
    PVOID VmxonR; 
    PVOID Stack; 
    PHYSICAL_ADDRESS lowest_acceptable_address = {0};
    PHYSICAL_ADDRESS boundary_address_multiple = {0};

    /*
    1.MmAllocateNonCachedMemory
    2.MmAllocateContiguousMemory
    3.MmAllocateContiguousNodeMemory
    windows10��������֤��������£������������쳣��
    */
    PhyAddr.QuadPart = -1;//MmAllocateNonCachedMemory 
    //VmxonR = MmAllocateContiguousMemory(PAGE_SIZE, PhyAddr);
    VmxonR = MmAllocateContiguousNodeMemory(PAGE_SIZE, lowest_acceptable_address, PhyAddr, boundary_address_multiple, PAGE_READWRITE, MM_ANY_NODE_OK);
    ASSERT (VmxonR);
    RtlZeroMemory (VmxonR, PAGE_SIZE);

    //Vmcs  = MmAllocateContiguousMemory(PAGE_SIZE, PhyAddr);
    Vmcs = MmAllocateContiguousNodeMemory(PAGE_SIZE, lowest_acceptable_address, PhyAddr, boundary_address_multiple, PAGE_READWRITE, MM_ANY_NODE_OK);
    ASSERT (Vmcs);
    RtlZeroMemory (Vmcs, PAGE_SIZE);

    Stack  = ExAllocatePoolWithTag (NonPagedPoolNx, 2 * PAGE_SIZE, TAG);ASSERT (Stack); RtlZeroMemory (Stack, 2 * PAGE_SIZE);

    set_cr4();

    *(SIZE_T *) VmxonR = (__readmsr(MSR_IA32_VMX_BASIC) & 0xffffffff); 
    *(SIZE_T *) Vmcs  = (__readmsr(MSR_IA32_VMX_BASIC) & 0xffffffff); 

    PhyAddr = MmGetPhysicalAddress(VmxonR);
    rc = __vmx_on ((unsigned __int64 *)&PhyAddr);ASSERT(!rc);

    PhyAddr = MmGetPhysicalAddress(Vmcs);
    rc = __vmx_vmclear ((unsigned __int64 *)&PhyAddr); ASSERT(!rc);
    rc = __vmx_vmptrld ((unsigned __int64 *)&PhyAddr); ASSERT(!rc);

    HostRsp = (SIZE_T)Stack + 2 * PAGE_SIZE - sizeof(void *);
    SetVMCS(HostRsp, GuestRsp);

    rc = __vmx_vmlaunch();
    ASSERT (0 == rc);
    if (0 != rc)
    {
        size_t FieldValue = 0;
        #define VM_instruction_error  0x00004400

        rc = __vmx_vmread(VM_instruction_error, &FieldValue);
        if (0 == rc)
        {
            //���ݴ����룺FieldValue�μ���30.4 VM INSTRUCTION ERROR NUMBERS
            //KdPrint(("__vmx_vmread VM_instruction_error FieldValue:0x%x. in line: %d at file:%s.\r\n", FieldValue, __LINE__, __FILE__));
        }

        __vmx_off();

        //reset_cr4();

        //�ͷ��ڴ档
    }
    
    return STATUS_SUCCESS;
}


BOOL is_support_blos()
    /*
    ���ܣ��ж��������Ƿ���vmx��
    ������λ�ṹ��
    https://msdn.microsoft.com/zh-cn/library/ewwyfdbe.aspx
    https://msdn.microsoft.com/zh-cn/library/yszfawxh.aspx
    Ҳ������ָ����ɣ�
    https://msdn.microsoft.com/en-us/library/h65k4tze.aspx

    Ȩ�����ϣ�    
    23.7 ENABLING AND ENTERING VMX OPERATION
    */
{
    SIZE_T MSR_IA32_FEATURE_CONTROL = 0;
    unsigned char b =  0;

    MSR_IA32_FEATURE_CONTROL = __readmsr(IA32_FEATURE_CONTROL);

    b = _bittest64(&MSR_IA32_FEATURE_CONTROL, 0);
    if (0 == b)
    {
        return FALSE;//If this bit is clear, VMXON causes a general-protection exception.
    }

    b = _bittest64(&MSR_IA32_FEATURE_CONTROL, 1);
    if (0 == b)
    {
        KdPrint(("SMX �²�֧�� VMX��������Ҳû��ϵ.\r\n"));
        //return FALSE;
    }

    b = _bittest64(&MSR_IA32_FEATURE_CONTROL, 2);
    if (0 == b)
    {
        return FALSE;
    }
    
    return TRUE;
}


BOOL is_support_vmx()
    /*
    ���ܣ��ж�CPU�Ƿ�֧��VMXָ�
    Ȩ�����ϣ�23.6 DISCOVERING SUPPORT FOR VMX

    System software can determine whether a processor supports VMX operation using CPUID.
    If CPUID.1:ECX.VMX[bit 5] = 1, then VMX operation is supported.
    */
{
    BOOL B = FALSE;
    int CPUInfo[4] = {-1};
    int ecx = 0;

    __cpuid(CPUInfo, 1);
    
    ecx = CPUInfo[2];

    B = _bittest((LONG const *)&ecx, 5);
    
    return B;
}


BOOL is_support_intel()
{
    BOOL B = FALSE;
    char CPUString[0x20];
    int CPUInfo[4] = {-1};
    unsigned    nIds;

    // __cpuid with an InfoType argument of 0 returns the number of valid Ids in CPUInfo[0] and the CPU identification string in the other three array elements.
    // The CPU identification string is not in linear order. 
    // The code below arranges the information in a human readable form.
    __cpuid(CPUInfo, 0);
    nIds = CPUInfo[0];
    memset(CPUString, 0, sizeof(CPUString));
    *((int*)CPUString) = CPUInfo[1];
    *((int*)(CPUString+4)) = CPUInfo[3];
    *((int*)(CPUString+8)) = CPUInfo[2];

    if (_stricmp(CPUString, "GenuineIntel") == 0)
    {
        B = TRUE;
    } 

    return B;
}


BOOL is_support_cpuid()
/*
�ж�CPU�Ƿ�֧��CPUIDָ�

The ID flag (bit 21) in the EFLAGS register indicates support for the CPUID instruction.
If a software procedure can set and clear this flag, the processor executing the procedure supports the CPUID instruction.
This instruction operates the same in non-64-bit modes and 64-bit mode.
*/
{
    BOOL B = FALSE;
    SIZE_T original;
    SIZE_T result;
    SIZE_T temp;

    original = __readeflags(); //��ȡ
    if (_bittest64(&original, 21) == 0)
    {
        temp = original;
        _bittestandset64(&temp, 21);//������һλΪ1. ��һ���취�Ǻ�0x200000���л������

        __writeeflags(temp);//д����ԡ�

        result = __readeflags();//��ȡ�����

        __writeeflags(original);//�ָ���

        //�ж�
        if (_bittest64(&result, 21) == 0) {
            B = FALSE;
        }
        else {
            B = TRUE;
        }
    }
    else
    {
        ASSERT(_bittest64(&original, 21) == 0);//�������λΪ0.
        //��ʵ��Ҫ��������������ơ�
    }

    return B;
}


VOID DriverUnload (PDRIVER_OBJECT DriverObject)
{
    KIRQL OldIrql;
    CHAR i;
    
    for (i = 0; i < KeNumberProcessors; i++)
    {
        KeSetSystemAffinityThread ((KAFFINITY) ((ULONG_PTR)1 << i));
        OldIrql = KeRaiseIrqlToDpcLevel ();
        VmxVmCall (NBP_HYPERCALL_UNLOAD);
        KeLowerIrql (OldIrql);
        KeRevertToUserAffinityThread ();
    }
}


NTSTATUS DriverEntry (PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS Status;
    KIRQL OldIrql;
    CHAR i;

    KdBreakPoint();

    if (!is_support_cpuid())
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (!is_support_intel()) //AMD���⻯�Ĺ��ܻ��д����롣
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (!is_support_vmx())
    {
        return STATUS_NOT_SUPPORTED;
    }

    //��ȡ0x480��MSR����Ϣ��

    if (!is_support_blos())
    {
        return STATUS_NOT_SUPPORTED;
    }

    //��ȡ0x48C��MSR����Ϣ���ж��Ƿ�֧��EPT��

    for (i = 0; i < KeNumberProcessors; i++)
    {
        KeSetSystemAffinityThread ((KAFFINITY) ((ULONG_PTR)1 << i));
        OldIrql = KeRaiseIrqlToDpcLevel ();
        Status = CmSubvert();//һ����ຯ���������Ǳ������мĴ���(���˶μĴ���)�����ݵ�ջ��󣬵���HvmSubvertCpu
        KeLowerIrql (OldIrql);
        KeRevertToUserAffinityThread ();
        ASSERT(NT_SUCCESS(Status));
    }

    DriverObject->DriverUnload = DriverUnload;
    return STATUS_SUCCESS;
}