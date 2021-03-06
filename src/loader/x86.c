#include "x86.h"

#include "buffer.h"
#include "function.h"
#include "index.h"


void * x86_graph_wqueue (struct _x86_wqueue * x86_wqueue)
{
    return x86_graph(x86_wqueue->address, x86_wqueue->memory);
}


void * x86_functions_wqueue (struct _x86_wqueue * x86_wqueue)
{
    return x86_functions(x86_wqueue->address, x86_wqueue->memory);
}


struct _ins * x86_ins (uint64_t address, ud_t * ud_obj)
{
    struct _ins * ins;
    ins = ins_create(address,
                     ud_insn_ptr(ud_obj),
                     ud_insn_len(ud_obj),
                     ud_insn_asm(ud_obj),
                     NULL);

    if (ud_obj->operand[0].type == UD_OP_JIMM) {
        char * mnemonic_str = NULL;
        switch (ud_obj->mnemonic) {
        case UD_Ijo   : mnemonic_str = "jo";   break;
        case UD_Ijno  : mnemonic_str = "jno";  break;
        case UD_Ijb   : mnemonic_str = "jb";   break;
        case UD_Ijae  : mnemonic_str = "jae";  break;
        case UD_Ijz   : mnemonic_str = "jz";   break;
        case UD_Ijnz  : mnemonic_str = "jnz";  break;
        case UD_Ijbe  : mnemonic_str = "jbe";  break;
        case UD_Ija   : mnemonic_str = "ja";   break;
        case UD_Ijs   : mnemonic_str = "js";   break;
        case UD_Ijns  : mnemonic_str = "jns";  break;
        case UD_Ijp   : mnemonic_str = "jp";   break;
        case UD_Ijnp  : mnemonic_str = "jnp";  break;
        case UD_Ijl   : mnemonic_str = "jl";   break;
        case UD_Ijge  : mnemonic_str = "jge";  break;
        case UD_Ijle  : mnemonic_str = "jle";  break;
        case UD_Ijg   : mnemonic_str = "jg";   break;
        case UD_Ijmp  : mnemonic_str = "jmp";  break;
        case UD_Iloop : mnemonic_str = "loop"; break;
        case UD_Icall : mnemonic_str = "call"; break;
        default :break;
        }

        if (mnemonic_str != NULL) {
            char tmp[64];
            uint64_t destination;
            destination  = address + ud_insn_len(ud_obj);
            destination += udis86_sign_extend_lval(&(ud_obj->operand[0]));
            snprintf(tmp, 64, "%s %llx", mnemonic_str,
                     (unsigned long long) destination);
            ins_s_description(ins, tmp);
            ins_s_target(ins, destination);
        }
    }
    else if (udis86_target(address, &(ud_obj->operand[1])) != -1) {
        uint64_t destination = ud_insn_len(ud_obj);
        destination += udis86_target(address, &(ud_obj->operand[1]));
        struct _reference * reference;
        reference = reference_create(REFERENCE_LOAD, address, destination);
        ins_add_reference(ins, reference);
        object_delete(reference);
    }
    else if (udis86_target(address, &(ud_obj->operand[0])) != -1) {
        uint64_t destination = ud_insn_len(ud_obj);
        destination += udis86_target(address, &(ud_obj->operand[1]));
        struct _reference * reference;
        reference = reference_create(REFERENCE_STORE, address, destination);
        ins_add_reference(ins, reference);
        object_delete(reference);
    }
    if (    (ud_obj->operand[0].type == UD_OP_IMM)
         && (ud_obj->operand[0].size >= 32)) {
        int64_t tmp = udis86_sign_extend_lval(&(ud_obj->operand[0]));
        if (tmp > 0x1000) {
            struct _reference * reference;
            reference = reference_create(REFERENCE_CONSTANT,
                                         address,
                                         udis86_sign_extend_lval(&(ud_obj->operand[0])));
            ins_add_reference(ins, reference);
            object_delete(reference);
        }
    }
    if (    (ud_obj->operand[1].type == UD_OP_IMM)
         && (ud_obj->operand[1].size >= 32)) {
        int64_t tmp = udis86_sign_extend_lval(&(ud_obj->operand[1]));
        if (tmp > 0x1000) {
            struct _reference * reference;
            reference = reference_create(REFERENCE_CONSTANT,
                                         address,
                                         udis86_sign_extend_lval(&(ud_obj->operand[1])));
            ins_add_reference(ins, reference);
            object_delete(reference);
        }
    }

    if (ud_obj->mnemonic == UD_Icall)
        ins_s_call(ins);

    return ins;
}


/*
* This is the initial phase, used to populate the graph with all reachable
* nodes. We will worry about fixing edges from jmp-like mnemonics later.
*/
void x86_graph_0 (struct _graph * graph,
                  uint64_t        address,
                  struct _map *   memory)
{
    ud_t            ud_obj;
    int             continue_disassembling = 1;
    uint64_t        last_address = -1;
    int             edge_type = INS_EDGE_NORMAL;

    struct _buffer * buffer = map_fetch_max(memory, address);

    if (buffer == NULL)
        return;

    uint64_t base_address   = map_fetch_max_key(memory, address);

    if (base_address + buffer->size < address)
        return;

    uint64_t offset = address - base_address;

    ud_init      (&ud_obj);
    ud_set_mode  (&ud_obj, 32);
    ud_set_syntax(&ud_obj, UD_SYN_INTEL);
    ud_set_input_buffer(&ud_obj, &(buffer->bytes[offset]), buffer->size - offset);

    while (continue_disassembling == 1) {

        size_t bytes_disassembled = ud_disassemble(&ud_obj);
        if (bytes_disassembled == 0) {
            break;
        }

        // even if we have already added this node, make sure we add the edge
        // from the preceeding node, in case this node was added from a jump
        // previously. otherwise we won't have an edge from its preceeding
        // instruction
        if (graph_fetch_node(graph, address) != NULL) {
            if (last_address != -1) {
                // not concerned if this call fails
                struct _ins_edge * ins_edge = ins_edge_create(edge_type);
                graph_add_edge(graph, last_address, address, ins_edge);
                object_delete(ins_edge);
            }
            break;
        }

        // create graph node for this instruction
        struct _ins * ins = x86_ins(address, &ud_obj);
        struct _list * ins_list = list_create();
        list_append(ins_list, ins);
        graph_add_node(graph, address, ins_list);
        object_delete(ins_list);
        object_delete(ins);

        // add edge from previous instruction to this instruction
        if (last_address != -1) {
            struct _ins_edge * ins_edge = ins_edge_create(edge_type);
            graph_add_edge(graph, last_address, address, ins_edge);
            object_delete(ins_edge);
        }

        // these mnemonics cause us to continue disassembly somewhere else
        struct ud_operand * operand;
        switch (ud_obj.mnemonic) {
        case UD_Ijo   :
        case UD_Ijno  :
        case UD_Ijb   :
        case UD_Ijae  :
        case UD_Ijz   :
        case UD_Ijnz  :
        case UD_Ijbe  :
        case UD_Ija   :
        case UD_Ijs   :
        case UD_Ijns  :
        case UD_Ijp   :
        case UD_Ijnp  :
        case UD_Ijl   :
        case UD_Ijge  :
        case UD_Ijle  :
        case UD_Ijg   :
        case UD_Ijmp  :
        case UD_Iloop :
            operand = &(ud_obj.operand[0]);

            if (operand->type != UD_OP_JIMM)
                break;

            if (ud_obj.mnemonic == UD_Icall)
                edge_type = INS_EDGE_NORMAL;
            else if (ud_obj.mnemonic == UD_Ijmp)
                edge_type = INS_EDGE_NORMAL; // not important, will terminate
            else
                edge_type = INS_EDGE_JCC_FALSE;

            if (operand->type == UD_OP_JIMM) {
                x86_graph_0(graph,
                            address
                              + ud_insn_len(&ud_obj)
                              + udis86_sign_extend_lval(operand),
                            memory);
            }
            break;
        default :
            edge_type = INS_EDGE_NORMAL;
            break;
        }

        // these mnemonics cause disassembly to stop
        switch (ud_obj.mnemonic) {
        case UD_Iret :
        case UD_Ihlt :
        case UD_Ijmp :
            continue_disassembling = 0;
            break;
        default :
            break;
        }

        last_address = address;
        address += bytes_disassembled;
    }
}


/*
* In this pass, we are fixing the edges from jmp-like instructions and their
* targets
*/
void x86_graph_1 (struct _graph * graph,
                  uint64_t        address)
{
    struct _graph_it * it;
    ud_t               ud_obj;

    for (it = graph_iterator(graph); it != NULL; it = graph_it_next(it)) {
        struct _list * ins_list = graph_it_data(it);
        struct _ins  * ins = list_first(ins_list);

        ud_init      (&ud_obj);
        ud_set_mode  (&ud_obj, 32);
        ud_set_input_buffer(&ud_obj, ins->bytes, ins->size);
        ud_disassemble(&ud_obj);

        struct ud_operand * operand;
        switch (ud_obj.mnemonic) {
        case UD_Ijmp  :
        case UD_Ijo   :
        case UD_Ijno  :
        case UD_Ijb   :
        case UD_Ijae  :
        case UD_Ijz   :
        case UD_Ijnz  :
        case UD_Ijbe  :
        case UD_Ija   :
        case UD_Ijs   :
        case UD_Ijns  :
        case UD_Ijp   :
        case UD_Ijnp  :
        case UD_Ijl   :
        case UD_Ijge  :
        case UD_Ijle  :
        case UD_Ijg   :
        case UD_Iloop :
            operand = &(ud_obj.operand[0]);

            if (operand->type != UD_OP_JIMM)
                break;
            
            uint64_t head = graph_it_index(it);
            uint64_t tail = head
                             + ud_insn_len(&ud_obj)
                             + udis86_sign_extend_lval(operand);

            int type = INS_EDGE_JCC_TRUE;
            if (ud_obj.mnemonic == UD_Ijmp)
                type = INS_EDGE_JUMP;

            struct _ins_edge * ins_edge = ins_edge_create(type);
            graph_add_edge(graph, head, tail, ins_edge);
            object_delete(ins_edge);
            break;
        default :
            break;
        }
    }
}



struct _graph * x86_graph (uint64_t address, struct _map * memory)
{
    struct _graph * graph;

    graph = graph_create();

    x86_graph_0(graph, address, memory);
    x86_graph_1(graph, address);

    return graph;
}



void x86_functions_r (struct _map  * functions,
                      struct _tree * disassembled,
                      uint64_t       address,
                      struct _map  * memory)
{
    ud_t ud_obj;
    int  continue_disassembling = 1;

    struct _buffer * buffer = map_fetch_max(memory, address);

    if (buffer == NULL)
        return;

    uint64_t base_address   = map_fetch_max_key(memory, address);

    if (base_address + buffer->size < address)
        return;

    uint64_t offset = address - base_address;

    ud_init      (&ud_obj);
    ud_set_mode  (&ud_obj, 32);
    ud_set_syntax(&ud_obj, UD_SYN_INTEL);
    ud_set_input_buffer(&ud_obj, &(buffer->bytes[offset]), buffer->size - offset);

    while (continue_disassembling == 1) {
        size_t bytes_disassembled = ud_disassemble(&ud_obj);
        if (bytes_disassembled == 0) {
            break;
        }

        if (    (ud_obj.mnemonic == UD_Icall)
             && (ud_obj.operand[0].type == UD_OP_JIMM)) {

            uint64_t target_addr = address
                                   + ud_insn_len(&ud_obj)
                                   + udis86_sign_extend_lval(&(ud_obj.operand[0]));

            if (map_fetch(functions, target_addr) == NULL) {
                struct _function * function = function_create(target_addr);
                map_insert(functions, target_addr, function);
                object_delete(function);
            }
        }

        struct _index * index = index_create(address);
        if (tree_fetch(disassembled, index) != NULL) {
            object_delete(index);
            return;
        }
        tree_insert(disassembled, index);
        object_delete(index);

        // these mnemonics cause us to continue disassembly somewhere else
        struct ud_operand * operand;
        switch (ud_obj.mnemonic) {
        case UD_Ijo   :
        case UD_Ijno  :
        case UD_Ijb   :
        case UD_Ijae  :
        case UD_Ijz   :
        case UD_Ijnz  :
        case UD_Ijbe  :
        case UD_Ija   :
        case UD_Ijs   :
        case UD_Ijns  :
        case UD_Ijp   :
        case UD_Ijnp  :
        case UD_Ijl   :
        case UD_Ijge  :
        case UD_Ijle  :
        case UD_Ijg   :
        case UD_Ijmp  :
        case UD_Iloop :
        case UD_Icall :
            operand = &(ud_obj.operand[0]);

            if (operand->type == UD_OP_JIMM) {
                x86_functions_r(functions,
                                disassembled,
                                address
                                 + ud_insn_len(&ud_obj)
                                 + udis86_sign_extend_lval(operand),
                                 memory);
            }
            break;
        default :
            break;
        }

        // these mnemonics cause disassembly to stop
        switch (ud_obj.mnemonic) {
        case UD_Iret :
        case UD_Ihlt :
        case UD_Ijmp :
            continue_disassembling = 0;
            break;
        default :
            break;
        }

        address += bytes_disassembled;
    }
}



struct _map * x86_functions (uint64_t address, struct _map * memory)
{

    struct _map  * functions    = map_create();
    struct _tree * disassembled = tree_create();

    x86_functions_r(functions, disassembled, address, memory);

    object_delete(disassembled);

    return functions;
}