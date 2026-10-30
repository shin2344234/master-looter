#pragma once
// Every byte pattern and struct offset the loot engine knows about the game.
// Nothing here is an absolute address: patterns are re-scanned at load and the
// Status tab shows which resolved. Offsets are validated at runtime where they
// can be (the item table is checked against our own item database before use).
//
// Provenance: CDLoot 1.0.4 (event protocol, entity layout, ownership oracle,
// node arming, verified on exe 1.0.0.2760, all still resolving on 1.0.0.2850)
// and Trinity (table resolver
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

    // void* alloc_event(0, u32 sizeCode). Static-TLS index in the prologue is a
    // wildcard. Since exe 1.0.0.3392 it calls the TLS init helper before reading
    // the thread block and inlines the event constructor; the first argument is
    // still spilled and never read, so alloc_event(0, size) is still the call.
    inline constexpr const char* kSig_AllocEvent =
        "48 89 5C 24 ?? 48 89 74 24 ?? 4C 89 44 24 ?? 48 89 4C 24 ?? 57 48 83 EC 30 8B F2 E8 ?? ?? ?? ?? "
        "BA ?? ?? 00 00 65 48 8B 04 25 58 00 00 00";

    // void enqueue(queue, event, desc, 0). Reads the TLS array in its prologue.
    // 1.0.0.3392 added the rsi spill and holds [rdx+0x38] in rdi.
    inline constexpr const char* kSig_Enqueue =
        "48 89 5C 24 08 48 89 74 24 20 57 48 83 EC 20 48 8B ?? 38 65 48 8B 04 25 58 00 00 00";

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
    // The same entry with another mod's five-byte jump over it, which is what
    // Trinity.asi does. The jump splits the second instruction, so both of its
    // leftover bytes are wildcards. The tail on its own matches six functions;
    // none of the other five has an E9 seven bytes before it.
    inline constexpr const char* kSig_MoveUpdateJumped = "E9 ?? ?? ?? ?? ?? ?? 48 89 50 ?? 55 41 56";

    // Fallback: the function CDLoot hooks as "area_sweep". The match lands 0x0F
    // bytes into the function on an instruction boundary (push rbp); it runs
    // thousands of times per second on the game thread. Register bytes after
    // the frame setup are wildcards.
    inline constexpr const char* kSig_AreaSweep =
        "55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 50 C5 F8 29 74 24 40 4C 8B ?? 48 8B ?? 48 8B ?? 44 8B";

    // --- Gimmick state machine ----------------------------------------------
    // void set_state(ClientGimmickActorComponent*, u32 stateNameId). Records the
    // new state at +0x270, bumps the change counter at +0x358 (+0x350 before
    // 1.0.0.3392, when the component grew eight bytes), writes +0x27C when the id
    // is one the node's own chart declares, then tail-calls the virtual at vtable
    // +0x880. It is not the whole transition: the state's exit and enter
    // actions run in the caller above it. Hooked only to watch, so the mod can
    // learn which layer a real pickaxe swing comes through, and with which ids.
    // The `E8` is the refcounted-temp helper and moves between builds, and the
    // counter's displacement is a wildcard so the pattern holds on both layouts.
    inline constexpr const char* kSig_SetState =
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 48 83 EC 30 8B F2 48 8B D9 48 8B 41 08 "
        "48 8D 50 08 33 FF 48 85 C0 48 0F 44 D7 41 B0 01 48 8B 12 48 8D 4C 24 20 E8 ?? ?? ?? ?? "
        "90 8B AB 70 02 00 00 89 B3 70 02 00 00 48 89 BB 80 02 00 00 FF 83 ?? 03 00 00";

    // The transition driver, vtable slot +0x7D8 of ClientGimmickActorComponent
    // (+0x7D0 before 1.0.0.3392, which added a slot at +0x2B0):
    //   drive(component, const Event* ev, void* instigator, bool* outChanged)
    // It does not take a target state. It takes an EVENT, asks the node's own
    // chart which transition that event fires from the current state, runs the
    // old state's exit actions, calls set_state once, then runs the new state's
    // enter actions. Hooked only to read the event id out of arg2, since that
    // id is the one thing neither the chart files nor the disassembly name.
    // 1.0.0.3392 stopped spilling rdx and moved the argument registers, so the
    // register bytes after the frame are wildcards.
    inline constexpr const char* kSig_StateDriver =
        "48 89 5C 24 18 4C 89 4C 24 20 55 56 57 41 54 41 55 41 56 41 57 "
        "48 8D AC 24 70 FF FF FF 48 81 EC 90 01 00 00 4D 8B ?? 4C 8B ?? 48 8B F9 "
        "33 ?? 89 ?? D0 00 00 00";

    // The game's GetInventoryHolder(actor): the holder of the bag an actor
    // actually uses. Most characters use their own, [[actor+0x68]+0xB8]. A
    // character whose row says so borrows another actor's instead: the actor
    // at [[actor+0xA0]+0xD0], and that actor's own [[+0x68]+0xB8]. Damiane and
    // Oongka do: their own holder is an empty 50-slot bag while everything
    // they pick up lands in the shared one. The mode comes from a character-table lookup that runs through
    // protected code, so the mod calls this rather than copying it, on the
    // game thread. The two calls are table lookups that move between builds.
    // 1.0.0.3392 passes the character row by value and moved both table
    // offsets (+0xBE and +0x5F before), so those two are wildcards.
    inline constexpr const char* kSig_InvHolder =
        "40 53 48 83 EC 20 48 8B 41 68 48 8B D9 48 8B 50 20 0F B7 52 30 E8 ?? ?? ?? ?? "
        "0F B7 90 ?? ?? 00 00 81 FA FF FF 00 00 75 04 B0 03 EB 09 E8 ?? ?? ?? ?? 0F B6 40 ?? 0F B6 C8 84 C0 74 ?? "
        "83 F9 01 75 ?? 48 8B 9B A0 00 00 00 48 85 DB 74 ?? 48 8B 9B D0 00 00 00";

    // How many item entities one drop-table row spawns. Watched, never called.
    //   if (!instigator) return 1;
    //   bonus = playerBuffFor(collectKey) + gimmickComponent[+0x1A0];
    //   return 1 + bonus/1000000 + (rand()%1000000 < bonus%1000000);
    // It has no direct callers: all three call sites reach it through a
    // one-instruction thunk, so hooking the body catches every drop in the game.
    // The pattern covers the prologue through the null-instigator early-out,
    // which is the branch worth watching.
    inline constexpr const char* kSig_DropCount =
        "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 "
        "44 89 C3 48 89 D7 48 89 CE 48 85 C9 75 0A B8 01 00 00 00 E9";

    // Walks a gimmick's drop table and calls the count function once per row that
    // passes the row filter. A pickaxe swing gets two calls out of this, a break
    // driven through the state machine gets one, so whatever it skips is the bug.
    // 1.0.0.3392 spills rbx instead of r9 and no longer pushes it.
    inline constexpr const char* kSig_DropRows =
        "48 89 5C 24 08 4C 89 44 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 "
        "48 8D AC 24 90 E7 FF FF B8 70 19 00 00";

    // The 0x0821 descriptor callback: resolves the vein and the player from the
    // payload and hands the vein's component to the spawn routine.
    inline constexpr const char* kSig_DropCallback =
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 70 48 8B DA 49 8B 78 18 49 8B 30 "
        "44 8B 47 03 48 8D 54 24 30";

    // --- Ownership oracle ---------------------------------------------------
    // bool own_check(ctx, player, target, tag, 7): the same call the game makes
    // to decide between "Take" and "Steal". ctx and tag are captured from the
    // game's own first calls. Pattern runs to the read of the 5th argument.
    inline constexpr const char* kSig_OwnCheck =
        "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8B EC "
        "48 81 EC 80 00 00 00 49 8B ?? 4C 8B ?? 4C 8B ?? 0F B6 7D 50";
    // Under another mod's entry jump, as Trinity.asi leaves it. The jump
    // replaces the first instruction exactly, and the rest is unique alone.
    inline constexpr const char* kSig_OwnCheckJumped =
        "E9 ?? ?? ?? ?? 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8B EC "
        "48 81 EC 80 00 00 00 49 8B ?? 4C 8B ?? 4C 8B ?? 0F B6 7D 50";
    // Under any other patch, found from the one call site that tests the
    // answer this way: mov rcx, [rcx+0x120]; call own_check; test al, al;
    // jz +5; mov dil, 2 (jz +4; mov bl, 2 before 1.0.0.3392). CDAutoLoot writes `mov rax, imm64; jmp rax` over the
    // first fifteen bytes, which neither pattern above survives. The call
    // names the function whatever its entry holds, and the body from +15 on,
    // past anything a fifteen-byte patch covers, confirms it. Both are unique
    // in exe 1.0.0.3392 and agree on +0x27FBCD0.
    inline constexpr const char* kSig_OwnCheckCaller = "48 8B 89 20 01 00 00 E8 ?? ?? ?? ?? 84 C0 74 05 40 B7 02";
    inline constexpr unsigned    kOff_OwnCheckCaller_Call = 7;
    inline constexpr const char* kSig_OwnCheckBody =
        "55 41 54 41 55 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00 49 8B ?? 4C 8B ?? 4C 8B ?? 0F B6 7D 50";
    inline constexpr unsigned    kOff_OwnCheckBody = 15;

    // --- The character being controlled ------------------------------------
    // The one call site that fetches the local actor through the game's
    // services object: mov rcx, [rip+X]; mov rcx, [rcx+10h]; call accessor;
    // nop; cmp [rbp+0xC0], r13b; je. Crimson Route resolves the controlled
    // character the same way. The accessor reads a controller at [rcx+58h]
    // and its actor at [controller+0D8h]; both displacements are read out of
    // its own instructions at run time. Unique in exe 1.0.0.3392 at
    // +0xB8F5C8, global +0x71ACBC8, accessor +0x99C250. The compare read
    // [rbp+0] before that build, which is the byte that broke.
    inline constexpr const char* kSig_ControlSite =
        "48 8B 0D ?? ?? ?? ?? 48 8B 49 10 E8 ?? ?? ?? ?? 90 44 38 AD C0 00 00 00 0F 84";
    inline constexpr unsigned kOff_ControlSite_Call = 11;
    inline constexpr unsigned kOff_ControlSite_Svc  = 0x10;

    // --- Node arming --------------------------------------------------------
    // void arm(gimmickComponent, u8 mode, const u32* nameId)
    //
    // Three arguments, not four: r9 is never spilled to its home slot and its
    // first touch at +0x205CD92 is a write, so whatever is passed there is
    // ignored. The third is dereferenced as a u32 at +0x205CD83 and used to
    // look up a row; point it at zero and the routine takes a broad path over
    // the node's own keys instead. It writes exactly one byte, the mode, onto
    // a named trigger, and lets the game react; on a lookup miss it writes
    // nothing and reports nothing, which is why a refused arm is silent.
    inline constexpr const char* kSig_ArmDispatch =
        "88 54 24 10 48 89 4C 24 08 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 58 49 8B ?? 44 0F B6 ?? 4C 8B ??";

    // --- Data tables --------------------------------------------------------
    // Table resolvers are ~120 clones of one template; each is told apart by the
    // `lea r8, [rip+"<tablename>"]` inside it. Since exe 1.0.0.3392 a resolver
    // takes its row by value in edx and builds its frame through r11:
    //   mov r11, rsp | mov [r11+8], rbx | mov [r11+0x18], rbp | push rsi, rdi,
    //   r14 | sub rsp, 0x60 | mov edi, edx | mov rbx, cs:<table global>
    // Three resolvers, characterinfo among them, sit in the .code section with
    // other encodings of the first and sixth instructions (49 89 E3, 89 D7),
    // so those bytes are wildcards. 130 resolvers match on 3392.
    inline constexpr const char* kSig_LeaR8Rip = "4C 8D 05 ?? ?? ?? ??";
    inline constexpr const char* kSig_TableResolver16 =
        "?? ?? ?? 49 89 5B 08 49 89 6B 18 56 57 41 56 48 83 EC ?? ?? ?? 48 8B 1D";
    inline constexpr unsigned kOff_TableResolver_MovGlobal = 0x15; // `mov rbx, cs:<table global>`

    // Every static table is addressed the same way: the table global is loaded,
    // the row is bounds-checked against the count at +8, then used to index the
    // def array. Anchoring on that sequence instead of one function prologue
    // enumerates every table in the image (154 on 1.0.0.3392), which is how an
    // unknown id can be tried against all of them. Before 3392 the row was read
    // with a movzx in front of the mov; it arrives in edx now, so the pattern
    // starts at the mov.
    //   mov rbx, cs:<table> | cmp r32, [rbx+8] | jae
    inline constexpr const char* kSig_TableIndex = "48 8B 1D ?? ?? ?? ?? 3B ?? 08 0F 83";
    inline constexpr unsigned kOff_TableIndex_MovGlobal = 0;   // 7-byte `mov rbx, [rip+disp]`
    inline constexpr unsigned kMax_LeaToPrologue = 0x180;

    inline constexpr const char* kStr_ItemInfoTable    = "iteminfo";
    inline constexpr const char* kStr_GimmickInfoTable = "gimmickinfo";
    // Table object: +0x08 u32 row count; def[] pointer at +0x50 (Trinity, build
    // 2026-08) or +0x58 (CDLoot, build 2760). Probed at runtime against our
    // item database; whichever offset yields matching string keys wins.
    inline constexpr unsigned kOff_Table_Count   = 0x08;
    inline constexpr unsigned kOff_Table_DefsA   = 0x58;
    inline constexpr unsigned kOff_Table_DefsB   = 0x50;
    // A def record opens with its string key since 1.0.0.3392, and the numeric
    // key follows it at +0x08; before that the key came first and the string
    // key sat at +0x08. Read from the loaders of ItemInfo, InventoryInfo,
    // InteractionInfo, CharacterInfo, StatusInfo and SkillInfo. GimmickInfo is
    // the exception: its loader stores the string key at +0x08 on 3392 (+0x10
    // on 2976, which the gimmick fallback below never accounted for).
    inline constexpr unsigned kOff_Def_StringKey        = 0x00; // -> string object -> char*
    inline constexpr unsigned kOff_GimmickDef_StringKey = 0x08;
    inline constexpr unsigned kOff_Def_Key16            = 0x08; // u16 _key where the key is 16 bits wide

    // --- RTTI anchors -------------------------------------------------------
    inline constexpr const char* kRtti_ActorManager = ".?AVClientActorManager@pa@@";
    inline constexpr const char* kCls_Status  = "ClientStatusActorComponent";
    inline constexpr const char* kCls_Gimmick = "ClientGimmickActorComponent";
    inline constexpr const char* kCls_Ai      = "ClientAiActorComponent";

    // The game asks a condition object whether a pet may loot, and the answer
    // is the last slot of that object's vtable. The slot before it is a stub
    // that hands back the condition's own label, which is what the install
    // searches for, so no slot index is written down here. Found 11 September
    // 2026 from the RTTI name in the exe, after the 2.02.00 patch moved the
    // address the old note recorded.
    //
    // One condition is not enough. The table row that lets a pet loot,
    // conditioninfo 33 (30 before 1.0.0.3392), InteractionTarget_PetLooting, reads:
    //
    //   (checkDead() && hasLootItem() && isLootable() && HasInteraction(Dead_Loot_Pet))
    //   || (IsPetLooting() && HasInteraction(Pet_LootingItem) && !IsCrimeTarget() && ...)
    //
    // The second half is a pet picking a loose item off the ground and the
    // first is a pet looting a body, and only the second passes through
    // IsPetLooting. The body half is refused through HasInteraction instead,
    // which keeps its argument as a 16-bit interaction row. Both Dead_Loot_Pet
    // and Pet_LootingItem appear in that one row of conditioninfo and nowhere
    // else in the table, so refusing the row cannot touch anything a person does.
    inline constexpr const char* kRtti_CondPetLooting     = ".?AVConditionData_IsPetLooting@pa@@";
    inline constexpr const char* kLabel_PetLooting        = "IsPetLooting()";
    inline constexpr const char* kRtti_CondHasInteraction = ".?AVConditionData_HasInteraction@pa@@";
    inline constexpr const char* kLabel_HasInteraction    = "HasInteraction()";
    // Position of Dead_Loot_Pet in interactioninfo.staticinfoheader, which is
    // the id the game passes around, the same convention as an item row: logs
    // from 1.0.0.2976 show the game asking about rows 29, 40 and 218, which are
    // Gimmick_Collect, Animal_Hunt and Pet_LootingItem at those positions. The
    // row is looked up by name at run time (game::RowByKey) and this is
    // only the fallback when the table cannot be read. 34 on 1.0.0.3392. The 12
    // written here before was Dead_Loot_Swim on 1.0.0.2976, where Dead_Loot_Pet
    // was 13, so on that build the body half of the pet switch refused the
    // wrong row.
    inline constexpr const char*    kStr_InteractionTable    = "interactioninfo";
    inline constexpr const char*    kRowKey_DeadLootPet      = "Dead_Loot_Pet";
    inline constexpr unsigned short kInteraction_DeadLootPet = 34;

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
    // 1.0.0.3392 put 0x20 bytes into the status component somewhere between
    // the creature row at +0x30, which still reads, and the dead flag. Byte
    // reads at +0x273 fell from 333 sites in 2976 to 22, +0x293 rose from 9 to
    // 334, and 104 of the old sites reappear byte for byte with +0x293 in them
    // against 2 with +0x273. cat2 votes the same way, 35 against 20. cat is
    // only ever logged and is assumed to move with them. Until then every
    // body read dead 0 and cat 00/00, so nothing was ever searched.
    inline constexpr unsigned kOff_Status_Dead   = 0x293; // u8 == 1 corpse (0x273 on 2976)
    inline constexpr unsigned kOff_Status_Cat    = 0x2E8; // u8 legacy category (quest 1, shop 0xF, decor 0x11) (0x2C8)
    inline constexpr unsigned kOff_Status_Cat2   = 0x2F0; // u8: 0x09 small creature, 0x05 fish, 0x0C beast, 0x11 worn (0x2D0)
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
    // The component grew eight bytes between +0x338 and +0x350 in 1.0.0.3392
    // and every field from +0x358 on moved by 8, so +0x3E2 became +0x3EA. The
    // constructor stores and set_state's +0x3E0 -> +0x3E8 show the shift; no
    // instruction reading this byte has been found in either build.
    inline constexpr unsigned kOff_Gimmick_Locked     = 0x3EA; // u8
    inline constexpr unsigned kOff_Mgr_ListsBegin = 0x100; // {u32 count, u32 cap, ptr} triples probed in
    inline constexpr unsigned kOff_Mgr_ListsEnd   = 0x200; // this window; the entity list sat at +0x190

    // Inventory holder walk (CDLoot, confirmed by Trinity's layout).
    inline constexpr unsigned kOff_Inv_Buckets   = 0x18;
    inline constexpr unsigned kOff_Inv_BucketN   = 0x20;
    inline constexpr unsigned kOff_Bucket_Slots  = 0x00;
    inline constexpr unsigned kOff_Bucket_SlotN  = 0x08; // u16
    inline constexpr unsigned kInv_SlotStride    = 0xC8;
    // The carried bag is bucket 0: the only one whose contents move as the
    // player picks things up. How full it is sits in two u16 fields side by
    // side, found on 2.01.00 by sampling the structure while the bag filled
    // and emptied (the bag stopped at 98 every time, and 98 is what +0x14
    // holds). Both are validated before use and the behaviour check stands in
    // when they read as nonsense.
    inline constexpr unsigned kInv_BagBucket    = 0;
    inline constexpr unsigned kOff_Bucket_Used  = 0x12; // u16, occupied slots
    inline constexpr unsigned kOff_Bucket_Cap   = 0x14; // u16, how many it holds
    inline constexpr unsigned kOff_Slot_TypeId   = 0x08; // u16, 0xFFFF empty

    // The game builds the ownership call's first argument the same way at six
    // of its nine call sites: mov rcx,[actor+0x68]; mov rcx,[rcx+0x120]. If
    // that chain gives the same pointer the game passes, the mod can work it
    // out from the player instead of waiting to watch a call go by. Observed
    // and logged for now, not used.
    inline constexpr unsigned kOff_Own_CtxHolder = 0x68;
    inline constexpr unsigned kOff_Own_CtxField  = 0x120;

    // The last instruction before the game calls the ownership routine, at six
    // of its nine call sites: mov rcx, [rcx+0x120]; call own_check. Finding it
    // gives the fourth argument, which every one of those six passes the same
    // pointer for, from a lea a little further back. With that and the context
    // above, the mod can ask the question itself instead of waiting to watch
    // the game ask it.
    inline constexpr const char* kSig_OwnCallSite = "48 8B 89 20 01 00 00 E8 ?? ?? ?? ??";
    inline constexpr unsigned kOff_OwnCallSite_Call = 7;    // the E8
    inline constexpr unsigned kMax_OwnCallSite_Back = 0x40; // how far back the lea r9 sits

    // --- Event body layout --------------------------------------------------
    // Issue #32, build 2760: the inventory delete routine that
    // TrocTrDeleteItemFromInventoryOnceTimer's handler calls. Reached by
    // address for one verbose-log hook that says how the game answered a
    // delete, which is what turned 0x73353994 into eErrNoInvalidInventory.
    inline constexpr unsigned kRva_InvDelete   = 0x0FD169C0;
    // Its first 38 bytes, so the hook is only ever placed on this build.
    inline constexpr const char* kSig_InvDeletePrologue = "49 89 E3 49 89 5B 08 49 89 6B 10 49 89 73 18 49 89 7B 20 41 54 41 56 41 57 48 83 EC 70 4D 89 C7 49 89 D6 48 89 CD";
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
