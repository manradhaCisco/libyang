/**
 * @file printer_xml.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief XML printer for libyang data structure
 *
 * Copyright (c) 2015 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>

#include "common.h"
#include "printer.h"
#include "xml_internal.h"
#include "tree_data.h"
#include "tree_schema.h"
#include "resolve.h"
#include "tree_internal.h"

#define INDENT ""
#define LEVEL (level ? level*2-2 : 0)

void xml_print_node(struct lyout *out, int level, const struct lyd_node *node, int toplevel);

struct mlist {
    struct mlist *next;
    struct lys_module *module;
} *mlist = NULL, *mlist_new;

static int
modlist_add(struct mlist **mlist, const struct lys_module *mod)
{
    struct mlist *iter;

    for (iter = *mlist; iter; iter = iter->next) {
        if (mod == iter->module) {
            break;
        }
    }

    if (!iter) {
        iter = malloc(sizeof *iter);
        if (!iter) {
            LOGMEM;
            return EXIT_FAILURE;
        }
        iter->next = *mlist;
        iter->module = (struct lys_module *)mod;
        *mlist = iter;
    }

    return EXIT_SUCCESS;
}

static void
xml_print_ns(struct lyout *out, const struct lyd_node *node)
{
    struct lyd_node *next, *cur, *node2;
    struct lyd_attr *attr;
    const struct lys_module *wdmod = NULL;
    struct mlist *mlist = NULL, *miter;

    assert(out);
    assert(node);

    /* add node attribute modules */
    for (attr = node->attr; attr; attr = attr->next) {
        if (modlist_add(&mlist, attr->module)) {
            goto print;
        }
    }

    /* add node children nodes and attribute modules */
    if (!(node->schema->nodetype & (LYS_LEAF | LYS_LEAFLIST | LYS_ANYXML))) {
        /* get with-defaults module */
        wdmod = ly_ctx_get_module(node->schema->module->ctx, "ietf-netconf-with-defaults", NULL);

        LY_TREE_FOR(node->child, node2) {
            LY_TREE_DFS_BEGIN(node2, next, cur) {
                if (cur->dflt && wdmod) {
                    if (modlist_add(&mlist, wdmod)) {
                        goto print;
                    }
                }
                for (attr = cur->attr; attr; attr = attr->next) {
                    if (modlist_add(&mlist, attr->module)) {
                        goto print;
                    }
                }
            LY_TREE_DFS_END(node2, next, cur)}
        }
    }

print:
    /* print used namespaces */
    while (mlist) {
        miter = mlist;
        mlist = mlist->next;

        ly_print(out, " xmlns:%s=\"%s\"", miter->module->prefix, miter->module->ns);
        free(miter);
    }
}

static void
xml_print_attrs(struct lyout *out, const struct lyd_node *node)
{
    struct lyd_attr *attr;
    const char **prefs, **nss;
    const char *xml_expr;
    uint32_t ns_count, i;
    int rpc_filter = 0;
    const struct lys_module *wdmod = NULL;

    /* with-defaults */
    if (node->dflt) {
        /* get with-defaults module */
        wdmod = ly_ctx_get_module(node->schema->module->ctx, "ietf-netconf-with-defaults", NULL);
        if (wdmod) {
            /* print attribute only if context include with-defaults schema */
            ly_print(out, " %s:default=\"true\"", wdmod->prefix);
        }
    }
    /* technically, check for the extension get-filter-element-attributes from ietf-netconf */
    if (!strcmp(node->schema->name, "filter")
            && (!strcmp(node->schema->module->name, "ietf-netconf") || !strcmp(node->schema->module->name, "notifications"))) {
        rpc_filter = 1;
    }

    for (attr = node->attr; attr; attr = attr->next) {
        if (rpc_filter && !strcmp(attr->name, "type")) {
            ly_print(out, " %s=\"", attr->name);
        } else if (rpc_filter && !strcmp(attr->name, "select")) {
            xml_expr = transform_json2xml(node->schema->module, attr->value, &prefs, &nss, &ns_count);
            if (!xml_expr) {
                /* error */
                ly_print(out, "\"(!error!)\"");
                return;
            }

            for (i = 0; i < ns_count; ++i) {
                ly_print(out, " xmlns:%s=\"%s\"", prefs[i], nss[i]);
            }
            free(prefs);
            free(nss);

            ly_print(out, " %s=\"", attr->name);
            lyxml_dump_text(out, xml_expr);
            ly_print(out, "\"");

            lydict_remove(node->schema->module->ctx, xml_expr);
            continue;
        } else {
            ly_print(out, " %s:%s=\"", attr->module->prefix, attr->name);
        }
        lyxml_dump_text(out, attr->value);
        ly_print(out, "\"");
    }
}

static void
xml_print_leaf(struct lyout *out, int level, const struct lyd_node *node, int toplevel)
{
    const struct lyd_node_leaf_list *leaf = (struct lyd_node_leaf_list *)node;
    const char *ns;
    const char **prefs, **nss;
    const char *xml_expr;
    uint32_t ns_count, i;

    if (toplevel || !node->parent || nscmp(node, node->parent)) {
        /* print "namespace" */
        ns = lyd_node_module(node)->ns;
        ly_print(out, "%*s<%s xmlns=\"%s\"", LEVEL, INDENT, node->schema->name, ns);
    } else {
        ly_print(out, "%*s<%s", LEVEL, INDENT, node->schema->name);
    }

    if (toplevel) {
        xml_print_ns(out, node);
    }

    xml_print_attrs(out, node);

    switch (leaf->value_type & LY_DATA_TYPE_MASK) {
    case LY_TYPE_BINARY:
    case LY_TYPE_STRING:
    case LY_TYPE_BITS:
    case LY_TYPE_ENUM:
    case LY_TYPE_BOOL:
    case LY_TYPE_DEC64:
    case LY_TYPE_INT8:
    case LY_TYPE_INT16:
    case LY_TYPE_INT32:
    case LY_TYPE_INT64:
    case LY_TYPE_UINT8:
    case LY_TYPE_UINT16:
    case LY_TYPE_UINT32:
    case LY_TYPE_UINT64:
    case LY_TYPE_UNION:
        if (!leaf->value_str || !leaf->value_str[0]) {
            ly_print(out, "/>");
        } else {
            ly_print(out, ">");
            lyxml_dump_text(out, leaf->value_str);
            ly_print(out, "</%s>", node->schema->name);
        }
        break;

    case LY_TYPE_IDENT:
    case LY_TYPE_INST:
        xml_expr = transform_json2xml(node->schema->module, ((struct lyd_node_leaf_list *)node)->value_str,
                                      &prefs, &nss, &ns_count);
        if (!xml_expr) {
            /* error */
            ly_print(out, "\"(!error!)\"");
            return;
        }

        for (i = 0; i < ns_count; ++i) {
            ly_print(out, " xmlns:%s=\"%s\"", prefs[i], nss[i]);
        }
        free(prefs);
        free(nss);

        if (xml_expr[0]) {
            ly_print(out, ">");
            lyxml_dump_text(out, xml_expr);
            ly_print(out, "</%s>", node->schema->name);
        } else {
            ly_print(out, "/>");
        }
        lydict_remove(node->schema->module->ctx, xml_expr);
        break;

    case LY_TYPE_LEAFREF:
            if (leaf->value.leafref) {
                ly_print(out, ">");
                lyxml_dump_text(out, ((struct lyd_node_leaf_list *)(leaf->value.leafref))->value_str);
                ly_print(out, "</%s>", node->schema->name);
            } else if (leaf->value_str) {
                if(strchr(leaf->value_str ,':')) {
                    xml_expr = transform_json2xml(node->schema->module, ((struct lyd_node_leaf_list *)node)->value_str,
                                                  &prefs, &nss, &ns_count);
                    if (!xml_expr) {
                        /* error */
                        ly_print(out, "\"(!error!)\"");
                        return;
                    }
                    
                    for (i = 0; i < ns_count; ++i) {
                        ly_print(out, " xmlns:%s=\"%s\"", prefs[i], nss[i]);
                    }
                    free(prefs);
                    free(nss);
                    
                    if (xml_expr[0]) {
                        ly_print(out, ">");
                        lyxml_dump_text(out, xml_expr);
                        ly_print(out, "</%s>", node->schema->name);
                    } else {
                        ly_print(out, "/>");
                    }
                    lydict_remove(node->schema->module->ctx, xml_expr);
                    break;
                } else {
                    ly_print(out, ">");
                    lyxml_dump_text(out, leaf->value_str);
                    ly_print(out, "</%s>", node->schema->name);
                }
            }else {
                ly_print(out, "</%s>", node->schema->name);
            }
            
            break;


    case LY_TYPE_EMPTY:
        ly_print(out, "/>");
        break;
    
    default:
        /* error */
        ly_print(out, "\"(!error!)\"");
    }

    if (level) {
        ly_print(out, "\n");
    }
}

static void
xml_print_container(struct lyout *out, int level, const struct lyd_node *node, int toplevel)
{
    struct lyd_node *child;
    const char *ns;

    if (toplevel || !node->parent || nscmp(node, node->parent)) {
        /* print "namespace" */
        ns = lyd_node_module(node)->ns;
        ly_print(out, "%*s<%s xmlns=\"%s\"", LEVEL, INDENT, node->schema->name, ns);
    } else {
        ly_print(out, "%*s<%s", LEVEL, INDENT, node->schema->name);
    }

    if (toplevel) {
        xml_print_ns(out, node);
    }

    xml_print_attrs(out, node);

    if (!node->child) {
        ly_print(out, "/>%s", level ? "\n" : "");
        return;
    }
    ly_print(out, ">%s", level ? "\n" : "");

    LY_TREE_FOR(node->child, child) {
        xml_print_node(out, level ? level + 1 : 0, child, 0);
    }

    ly_print(out, "%*s</%s>%s", LEVEL, INDENT, node->schema->name, level ? "\n" : "");
}

static void
xml_print_list(struct lyout *out, int level, const struct lyd_node *node, int is_list, int toplevel)
{
    struct lyd_node *child;
    const char *ns;

    if (is_list) {
        /* list print */
        if (toplevel || !node->parent || nscmp(node, node->parent)) {
            /* print "namespace" */
            ns = lyd_node_module(node)->ns;
            ly_print(out, "%*s<%s xmlns=\"%s\"", LEVEL, INDENT, node->schema->name, ns);
        } else {
            ly_print(out, "%*s<%s", LEVEL, INDENT, node->schema->name);
        }

        if (toplevel) {
            xml_print_ns(out, node);
        }
        xml_print_attrs(out, node);

        if (!node->child) {
            ly_print(out, "/>%s", level ? "\n" : "");
            return;
        }
        ly_print(out, ">%s", level ? "\n" : "");

        LY_TREE_FOR(node->child, child) {
            xml_print_node(out, level ? level + 1 : 0, child, 0);
        }

        ly_print(out, "%*s</%s>%s", LEVEL, INDENT, node->schema->name, level ? "\n" : "");
    } else {
        /* leaf-list print */
        xml_print_leaf(out, level, node, toplevel);
    }
}

static void
xml_print_anyxml(struct lyout *out, int level, const struct lyd_node *node, int toplevel)
{
    char *buf;
    struct lyd_node_anyxml *axml = (struct lyd_node_anyxml *)node;
    const char *ns;

    if (toplevel || !node->parent || nscmp(node, node->parent)) {
        /* print "namespace" */
        ns = lyd_node_module(node)->ns;
        ly_print(out, "%*s<%s xmlns=\"%s\"", LEVEL, INDENT, node->schema->name, ns);
    } else {
        ly_print(out, "%*s<%s", LEVEL, INDENT, node->schema->name);
    }

    if (toplevel) {
        xml_print_ns(out, node);
    }
    xml_print_attrs(out, node);
    ly_print(out, ">");

    if (axml->xml_struct) {
        if (axml->value.xml) {
            lyxml_print_mem(&buf, axml->value.xml, LYXML_PRINT_FORMAT | LYXML_PRINT_SIBLINGS);
            ly_print(out, "\n%s", buf);
            free(buf);
        }
    } else {
        if (axml->value.str) {
            ly_print(out, "%s", axml->value.str);
        }
    }

    /* closing tag */
    ly_print(out, "%*s</%s>%s", LEVEL, INDENT, node->schema->name, level ? "\n" : "");
}

void
xml_print_node(struct lyout *out, int level, const struct lyd_node *node, int toplevel)
{
    switch (node->schema->nodetype) {
    case LYS_NOTIF:
    case LYS_RPC:
    case LYS_CONTAINER:
        xml_print_container(out, level, node, toplevel);
        break;
    case LYS_LEAF:
        xml_print_leaf(out, level, node, toplevel);
        break;
    case LYS_LEAFLIST:
        xml_print_list(out, level, node, 0, toplevel);
        break;
    case LYS_LIST:
        xml_print_list(out, level, node, 1, toplevel);
        break;
    case LYS_ANYXML:
        xml_print_anyxml(out, level, node, toplevel);
        break;
    default:
        LOGINT;
        break;
    }
}

int
xml_print_data(struct lyout *out, const struct lyd_node *root, int options)
{
    const struct lyd_node *node;

    /* content */
    LY_TREE_FOR(root, node) {
        xml_print_node(out, (options & LYP_FORMAT ? 1 : 0), node, 1);
        if (!(options & LYP_WITHSIBLINGS)) {
            break;
        }
    }
    ly_print_flush(out);

    return EXIT_SUCCESS;
}

