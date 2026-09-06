#pragma once
// Every byte pattern and struct offset the loot engine knows about the game.
// Nothing here is an absolute address: patterns are re-scanned at load and the
// Status tab shows which resolved. Offsets are validated at runtime where they
// can be (the item table is checked against our own item database before use).
//
// Provenance: CDLoot 1.0.4 (event protocol, entity layout, ownership oracle,
// node arming, verified on exe 1.0.0.2760) and Trinity (table resolver
// anchoring, movement tick, guarded memory idioms). mod/scripts/sigcheck.py
// parses this file and reports hit counts against the installed exe, so keep
// the `kSig_` literals on single lines or as adjacent string literals only.

namespace ml::sig
{
    // --- Event system -------------------------------------------------------
    // tls_init followed 0x60 bytes later by desc_lookup (the two are matched
    // together because tls_init alone matches three times). The TLS index in
    // `mov edx, imm32` and the register byte at the very end move between
    // builds, so both are wildcards.
    inline constexpr const char* kSig_TlsDesc =
        "48 83 EC 28 BA ?? 00 00 00 65 48 8B 04 25 58 00 00 00 48 8B 08 8B 04 0A 39 05 ?? ?? ?? ?? "
        "7E ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? FF 75 ?? 48 8D 0D ?? ?? ?? ?? "
        "E8 ?? ?? ?? ?? 90 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 83 C4 28 C3 "
        "CC CC CC CC CC CC 40 56 45 33 C9 44 0F B7 ??";
    inline constexpr unsigned kOff_TlsDesc_DescLookup = 0x60;

    // void* alloc_event(0, u32 sizeCode). Static-TLS index in the prologue is a wildcard.
    inline constexpr const char* kSig_AllocEvent =
        "48 89 5C 24 ?? 4C 89 44 24 ?? 57 48 83 EC 20 8B ?? BA ?? ?? 00 00";

    // void enqueue(queue, event, desc, 0). Reads the TLS array in its prologue.
    inline constexpr const char* kSig_Enqueue =
        "48 89 5C 24 08 57 48 83 EC 20 48 8B ?? 38 65 48 8B 04 25 58 00 00 00";

    // A call site that loads DESC_MASK (mov r8d,[rip]) and the event queue
    // global (mov r12,[rip]) around two calls. Both globals move every patch.
    inline constexpr const char* kSig_DescMaskQueue =
        "E8 ?? ?? ?? ?? 44 8B 05 ?? ?? ?? ?? 0F B7 54 24 ?? E8 ?? ?? ?? ?? 4C 8B 25";
    inline constexpr unsigned kOff_DescMask_Mov = 5;   // 7-byte `mov r8d, [rip+disp]`
    inline constexpr unsigned kOff_Queue_Mov    = 22;  // 7-byte `mov r12, [rip+disp]`

    // --- Game-thread pumps --------------------------------------------------
    // Primary: the per-tick movement integrator (Trinity's kSig_MoveUpdate).
    // Fires once per frame for the player on the game thread.
    inline constexpr const char* kSig_MoveUpdate = "48 8B C4 4C 89 48 ?? 48 89 50 ?? 55 41 56";

    // Fallback: the function CDLoot hooks as "area_sweep". The match lands 0x0F
    // bytes into the function on an instruction boundary (push rbp); it runs
    // thousands of times per second on the game thread. Register bytes after
    // the frame setup are wildcards.
    inline constexpr const char* kSig_AreaSweep =
        "55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 50 C5 F8 29 74 24 40 4C 8B ?? 48 8B ?? 48 8B ?? 44 8B";

    // --- Ownership oracle ---------------------------------------------------
    // bool own_check(ctx, player, target, tag, 7): the same call the game makes
    // to decide between "Take" and "Steal". ctx and tag are captured from the
    // game's own first calls. Pattern runs to the read of the 5th argument.
    inline constexpr const char* kSig_OwnCheck =
        "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8B EC "
        "48 81 EC 80 00 00 00 49 8B ?? 4C 8B ?? 4C 8B ?? 0F B6 7D 50";

    // --- Node arming --------------------------------------------------------
    // void arm(gimmickComponent, u8 mode, void* scratch, u32 playerEid): makes
    // the game fill a node's interaction data as if the player stood on it.
    inline constexpr const char* kSig_ArmDispatch =
        "88 54 24 10 48 89 4C 24 08 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 58 49 8B ?? 44 0F B6 ?? 4C 8B ??";

    // --- Data tables --------------------------------------------------------
    // Table resolvers are ~120 clones of one template; each is told apart by the
    // `lea r8, [rip+"<tablename>"]` inside it. Prologue with a 16-bit key
    // (iteminfo, gimmickinfo). Frame size is a wildcard: 0x50 on 2760.
    inline constexpr const char* kSig_LeaR8Rip = "4C 8D 05 ?? ?? ?? ??";
    inline constexpr const char* kSig_TableResolver16 =
        "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC ?? 0F B7 39 48 8B 1D";
    inline constexpr unsigned kOff_TableResolver_MovGlobal = 0x15; // `mov rbx, cs:<table global>`

    // Every static table is addressed the same way: a u16 row index is read
    // from the handle, bounds-checked against the count at +8, then used to
    // index the def array. Anchoring on that sequence instead of one function
    // prologue enumerates every table in the image (123 on 2760), which is how
    // an unknown id can be tried against all of them.
    //   movzx r32, word ptr [rcx] | mov r64, cs:<table> | cmp r32, [r64+8] | jae
    inline constexpr const char* kSig_TableIndex = "0F B7 ?? 48 8B ?? ?? ?? ?? ?? 3B ?? 08 0F 83";
    inline constexpr unsigned kOff_TableIndex_MovGlobal = 3;   // 7-byte `mov r64, [rip+disp]`
    inline constexpr unsigned kMax_LeaToPrologue = 0x180;

    inline constexpr const char* kStr_ItemInfoTable    = "iteminfo";
    inline constexpr const char* kStr_GimmickInfoTable = "gimmickinfo";

    // Table object: +0x08 u32 row count; def[] pointer at +0x50 (Trinity, build
    // 2026-08) or +0x58 (CDLoot, build 2760). Probed at runtime against our
    // item database; whichever offset yields matching string keys wins.
    inline constexpr unsigned kOff_Table_Count   = 0x08;
    inline constexpr unsigned kOff_Table_DefsA   = 0x58;
    inline constexpr unsigned kOff_Table_DefsB   = 0x50;
    inline constexpr unsigned kOff_Def_StringKey = 0x08; // -> string object -> char*

    // --- RTTI anchors -------------------------------------------------------
    inline constexpr const char* kRtti_ActorManager = ".?AVClientActorManager@pa@@";
    inline constexpr const char* kCls_Status  = "ClientStatusActorComponent";
    inline constexpr const char* kCls_Gimmick = "ClientGimmickActorComponent";
    inline constexpr const char* kCls_Ai      = "ClientAiActorComponent";

    // Event descriptor classes (ids are looked up by class name at runtime).
    inline constexpr const char* kDesc_Search = "TrocTrProcessLootingDeadDropOnceTimer";
    inline constexpr const char* kDesc_PickUp = "TrocTrProcessPickUpItemOnceTimer";
    inline constexpr const char* kDesc_Catch  = "TrocTrPushCharacterToInventoryOnceTimer";

    // --- Entity layout (2474 through 2760) ----------------------------------
    inline constexpr unsigned kOff_Ent_Eid       = 0x60; // u32; top byte 0xA0 player, 0xB0 world object
    inline constexpr unsigned kOff_Ent_Comps     = 0x68; // -> component slot array
    inline constexpr unsigned kOff_Ent_TypeInfo  = 0x88; // -> descriptor; tag byte at +1 (6 item, 7 plant)
    inline constexpr unsigned kOff_Ent_Route     = 0x90; // u32 session/route id
    inline constexpr unsigned kComps_SlotsEnd    = 0x80; // component pointers live in [0, 0x80)
    inline constexpr unsigned kOff_Comps_Transform = 0x1A0;
    inline constexpr unsigned kOff_Comps_InvHolder = 0xB8;
    inline constexpr unsigned kOff_Tf_Pos        = 0xB4; // 3 floats, parent-relative
    inline constexpr unsigned kOff_Tf_ParentEid  = 0xC8; // u32, 0xFFFFFFFF none
    inline constexpr unsigned kOff_Tf_ParentPos  = 0xEC; // 3 floats, parent world position
    inline constexpr unsigned kOff_Status_Dead   = 0x273; // u8 == 1 corpse
    inline constexpr unsigned kOff_Status_Cat    = 0x2C8; // u8 legacy category (quest 1, shop 0xF, decor 0x11)
    inline constexpr unsigned kOff_Status_Cat2   = 0x2D0; // u8: 0x09 small creature, 0x05 fish, 0x0C beast, 0x11 worn
    inline constexpr unsigned kOff_Gimmick_ItemData   = 0xC0; // -> {u32 instanceId, .., u16 typeId @+8}
    inline constexpr unsigned kOff_Gimmick_GatherData = 0xE0; // -> {u16 typeId, .., u8 kind @+5}
    inline constexpr unsigned kOff_Gimmick_NodeName   = 0x68; // -> string object -> char*
    // The node's own prefab path, the only durable name it has: the reported
    // type id changes between sessions. Two routes to the same string; the
    // first is the one every node in the 2026-09-06 probe answered on.
    inline constexpr unsigned kOff_Gimmick_Prefab     = 0x18; // -> object
    inline constexpr unsigned kOff_Prefab_Path        = 0x38; // -> string object -> char*
    inline constexpr unsigned kOff_Gimmick_PrefabAlt  = 0x58;
    inline constexpr unsigned kOff_PrefabAlt_Path     = 0x18;
    inline constexpr unsigned kOff_Gimmick_Locked     = 0x3E2; // u8
    inline constexpr unsigned kOff_Mgr_ListsBegin = 0x100; // {u32 count, u32 cap, ptr} triples probed in
    inline constexpr unsigned kOff_Mgr_ListsEnd   = 0x200; // this window; the entity list sat at +0x190

    // Inventory holder walk (CDLoot, confirmed by Trinity's layout).
    inline constexpr unsigned kOff_Inv_Buckets   = 0x18;
    inline constexpr unsigned kOff_Inv_BucketN   = 0x20;
    inline constexpr unsigned kOff_Bucket_Slots  = 0x00;
    inline constexpr unsigned kOff_Bucket_SlotN  = 0x08; // u16
    inline constexpr unsigned kInv_SlotStride    = 0xC8;
    inline constexpr unsigned kOff_Slot_TypeId   = 0x08; // u16, 0xFFFF empty

    // --- Event body layout --------------------------------------------------
    inline constexpr unsigned kOff_Ev_One      = 0x30;
    inline constexpr unsigned kOff_Ev_Zero40   = 0x40;
    inline constexpr unsigned kOff_Ev_Zero48   = 0x48;
    inline constexpr unsigned kOff_Ev_Player   = 0x50;
    inline constexpr unsigned kOff_Ev_Route    = 0x58;
    inline constexpr unsigned kOff_Ev_Desc     = 0x60;
    inline constexpr unsigned kOff_Ev_Size     = 0x68;
    inline constexpr unsigned kOff_Ev_Buffer   = 0x70;
    inline constexpr unsigned kOff_Ev_Flag78   = 0x78;
    inline constexpr unsigned kOff_Desc_Size   = 0x18; // u16 payload size on the descriptor
}
