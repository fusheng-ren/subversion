/*
 * branch.c : Element-Based Branching and Move Tracking.
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <stddef.h>
#include <assert.h>

#include "svn_types.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_hash.h"
#include "svn_props.h"
#include "svn_iter.h"

#include "private/svn_element.h"
#include "private/svn_branch.h"
#include "private/svn_sorts_private.h"
#include "svn_private_config.h"


#define FAMILY_HAS_BSID(family, bsid) \
  ((bsid) >= (family)->first_bsid && (bsid) < (family)->next_bsid)

#define FAMILY_HAS_ELEMENT(family, eid) \
  ((eid) >= (family)->first_eid && (eid) < (family)->next_eid)

#define BRANCH_FAMILY_HAS_ELEMENT(branch, eid) \
   FAMILY_HAS_ELEMENT((branch)->sibling_defn->family, (eid))

#define IS_BRANCH_ROOT_EID(branch, eid) \
  ((eid) == (branch)->sibling_defn->root_eid)

/* Is BRANCH1 the same branch as BRANCH2? Compare by full branch-ids; don't
   require identical branch-instance objects. */
#define BRANCH_IS_SAME_BRANCH(branch1, branch2, scratch_pool) \
  (strcmp(svn_branch_instance_get_id(branch1, scratch_pool), \
          svn_branch_instance_get_id(branch2, scratch_pool)) == 0)

/* Is BRANCH1 an immediate child of BRANCH2? Compare by full branch-ids; don't
   require identical branch-instance objects. */
#define BRANCH_IS_CHILD_OF_BRANCH(branch1, branch2, scratch_pool) \
  ((branch1)->outer_branch && \
   BRANCH_IS_SAME_BRANCH((branch1)->outer_branch, branch2, scratch_pool))

svn_branch_repos_t *
svn_branch_repos_create(apr_pool_t *result_pool)
{
  svn_branch_repos_t *repos = apr_pcalloc(result_pool, sizeof(*repos));

  repos->rev_roots = svn_array_make(result_pool);
  repos->pool = result_pool;
  return repos;
}

svn_branch_revision_root_t *
svn_branch_revision_root_create(svn_branch_repos_t *repos,
                                svn_revnum_t rev,
                                struct svn_branch_instance_t *root_branch,
                                apr_pool_t *result_pool)
{
  svn_branch_revision_root_t *rev_root
    = apr_pcalloc(result_pool, sizeof(*rev_root));

  rev_root->repos = repos;
  rev_root->rev = rev;
  rev_root->root_branch = root_branch;
  rev_root->branch_instances = svn_array_make(result_pool);
  return rev_root;
}

/* Assert FAMILY satisfies all its invariants.
 */
static void
assert_branch_family_invariants(const svn_branch_family_t *family)
{
  assert(family->branch_siblings);
  /* ### ... */
}

svn_branch_family_t *
svn_branch_family_create(svn_branch_repos_t *repos,
                         int first_bsid,
                         int next_bsid,
                         int first_eid,
                         int next_eid,
                         apr_pool_t *result_pool)
{
  svn_branch_family_t *f = apr_pcalloc(result_pool, sizeof(*f));

  f->repos = repos;
  f->branch_siblings = svn_array_make(result_pool);
  f->first_bsid = first_bsid;
  f->next_bsid = next_bsid;
  f->first_eid = first_eid;
  f->next_eid = next_eid;
  f->pool = result_pool;
  assert_branch_family_invariants(f);
  return f;
}

int
svn_branch_family_add_new_element(svn_branch_family_t *family)
{
  int eid = family->next_eid++;

  assert_branch_family_invariants(family);
  return eid;
}

/* Create a new branch sibling in FAMILY, with branch sibling id BSID and
 * root element ROOT_EID, and register it as a member of the family.
 */
static svn_branch_sibling_t *
family_create_branch_sibling(svn_branch_family_t *family,
                             int bsid,
                             int root_eid)
{
  svn_branch_sibling_t *branch_sibling
    = svn_branch_sibling_create(family, bsid, root_eid, family->pool);

  /* The root EID must be an existing EID. */
  SVN_ERR_ASSERT_NO_RETURN(root_eid >= family->first_eid
                           /*&& root_eid < family->next_eid*/);
  /* ROOT_RRPATH must not be within another branch of the family. */

  /* Register the branch */
  SVN_ARRAY_PUSH(family->branch_siblings) = branch_sibling;

  assert_branch_family_invariants(family);
  return branch_sibling;
}

/* Return the branch sibling definition with branch sibling id BSID in FAMILY.
 *
 * Return NULL if not found.
 */
static svn_branch_sibling_t *
family_find_branch_sibling(svn_branch_family_t *family,
                           int bsid)
{
  SVN_ITER_T(svn_branch_sibling_t) *si;

  for (SVN_ARRAY_ITER_NO_POOL(si, family->branch_siblings))
    if (si->val->bsid == bsid)
      return si->val;
  return NULL;
}

/* Return an existing (if found) or new (otherwise) branch sibling
 * definition object with id BSID and root-eid ROOT_EID in FAMILY.
 */
static svn_branch_sibling_t *
family_find_or_create_branch_sibling(svn_branch_family_t *family,
                                     int bsid,
                                     int root_eid)
{
  svn_branch_sibling_t *sibling = family_find_branch_sibling(family, bsid);

  if (!sibling)
    {
      sibling = family_create_branch_sibling(family, bsid, root_eid);
    }

  SVN_ERR_ASSERT_NO_RETURN(sibling->root_eid == root_eid);
  return sibling;
}

svn_branch_sibling_t *
svn_branch_family_add_new_branch_sibling(svn_branch_family_t *family,
                                         int root_eid)
{
  int bsid = family->next_bsid++;
  svn_branch_sibling_t *branch_sibling
    = family_create_branch_sibling(family, bsid, root_eid);

  assert_branch_family_invariants(family);
  return branch_sibling;
}

apr_array_header_t *
svn_branch_family_get_branch_instances(
                                svn_branch_revision_root_t *rev_root,
                                svn_branch_family_t *family,
                                apr_pool_t *result_pool)
{
  svn_array_t *fam_branch_instances = svn_array_make(result_pool);
  SVN_ITER_T(svn_branch_instance_t) *bi;

  for (SVN_ARRAY_ITER_NO_POOL(bi, rev_root->branch_instances))
    if (bi->val->sibling_defn->family == family)
      SVN_ARRAY_PUSH(fam_branch_instances) = bi->val;
  return fam_branch_instances;
}

/* Assert SIBLING satisfies all its invariants.
 */
static void
assert_branch_sibling_invariants(const svn_branch_sibling_t *sibling,
                                 apr_pool_t *scratch_pool)
{
  assert(sibling->family);
  assert(FAMILY_HAS_BSID(sibling->family, sibling->bsid));
  assert(FAMILY_HAS_ELEMENT(sibling->family, sibling->root_eid));
}

svn_branch_sibling_t *
svn_branch_sibling_create(svn_branch_family_t *family,
                             int bsid,
                             int root_eid,
                             apr_pool_t *result_pool)
{
  svn_branch_sibling_t *b = apr_pcalloc(result_pool, sizeof(*b));

  assert(FAMILY_HAS_BSID(family, bsid));
  assert(FAMILY_HAS_ELEMENT(family, root_eid));

  b->family = family;
  b->bsid = bsid;
  b->root_eid = root_eid;
  assert_branch_sibling_invariants(b, result_pool);
  return b;
}

/* Assert BRANCH satisfies all its invariants.
 */
static void
assert_branch_instance_invariants(const svn_branch_instance_t *branch,
                                  apr_pool_t *scratch_pool)
{
  assert(branch->sibling_defn);
  assert(branch->rev_root);
  if (branch->outer_branch)
    {
      assert(branch->outer_eid != -1);
      assert(BRANCH_FAMILY_HAS_ELEMENT(branch, branch->outer_eid));
    }
  else
    {
      assert(branch->outer_eid == -1);
    }
  assert(branch->e_map);
}

svn_branch_instance_t *
svn_branch_instance_create(svn_branch_sibling_t *branch_sibling,
                           svn_branch_revision_root_t *rev_root,
                           svn_branch_instance_t *outer_branch,
                           int outer_eid,
                           apr_pool_t *result_pool)
{
  svn_branch_instance_t *b = apr_pcalloc(result_pool, sizeof(*b));

  b->sibling_defn = branch_sibling;
  b->rev_root = rev_root;
  b->e_map = apr_hash_make(result_pool);
  b->outer_branch = outer_branch;
  b->outer_eid = outer_eid;
  assert_branch_instance_invariants(b, result_pool);
  return b;
}

svn_branch_el_rev_id_t *
svn_branch_el_rev_id_create(svn_branch_instance_t *branch,
                            int eid,
                            svn_revnum_t rev,
                            apr_pool_t *result_pool)
{
  svn_branch_el_rev_id_t *id = apr_palloc(result_pool, sizeof(*id));

  id->branch = branch;
  id->eid = eid;
  id->rev = rev;
  return id;
}

svn_branch_el_rev_content_t *
svn_branch_el_rev_content_create(svn_branch_eid_t parent_eid,
                                 const char *name,
                                 const svn_element_content_t *node_content,
                                 apr_pool_t *result_pool)
{
  svn_branch_el_rev_content_t *content
     = apr_palloc(result_pool, sizeof(*content));

  content->parent_eid = parent_eid;
  content->name = apr_pstrdup(result_pool, name);
  content->content = svn_element_content_dup(node_content, result_pool);
  return content;
}

svn_branch_el_rev_content_t *
svn_branch_el_rev_content_dup(const svn_branch_el_rev_content_t *old,
                              apr_pool_t *result_pool)
{
  svn_branch_el_rev_content_t *content
     = apr_pmemdup(result_pool, old, sizeof(*content));

  content->name = apr_pstrdup(result_pool, old->name);
  content->content = svn_element_content_dup(old->content, result_pool);
  return content;
}

svn_boolean_t
svn_branch_el_rev_content_equal(const svn_branch_el_rev_content_t *content_left,
                                const svn_branch_el_rev_content_t *content_right,
                                apr_pool_t *scratch_pool)
{
  if (!content_left && !content_right)
    {
      return TRUE;
    }
  else if (!content_left || !content_right)
    {
      return FALSE;
    }

  if (content_left->parent_eid != content_right->parent_eid)
    {
      return FALSE;
    }
  if (strcmp(content_left->name, content_right->name) != 0)
    {
      return FALSE;
    }
  if (! svn_element_content_equal(content_left->content,
                                       content_right->content,
                                       scratch_pool))
    {
      return FALSE;
    }

  return TRUE;
}


/*
 * ========================================================================
 * Branch mappings
 * ========================================================================
 */

/* Validate that NODE is suitable for a mapping of BRANCH:EID.
 * NODE->content may be null.
 */
static void
branch_map_node_validate(const svn_branch_instance_t *branch,
                         int eid,
                         const svn_branch_el_rev_content_t *node)
{
  SVN_ERR_ASSERT_NO_RETURN(node);

  /* Parent EID must be valid and different from this node's EID, or -1
     iff this is the branch root element. */
  SVN_ERR_ASSERT_NO_RETURN(
    IS_BRANCH_ROOT_EID(branch, eid)
    ? (node->parent_eid == -1)
    : (node->parent_eid != eid
       && BRANCH_FAMILY_HAS_ELEMENT(branch, node->parent_eid)));

  /* Node name must be given, and empty iff EID is the branch root. */
  SVN_ERR_ASSERT_NO_RETURN(
    node->name
    && IS_BRANCH_ROOT_EID(branch, eid) == (*node->name == '\0'));

  /* Content, if specified, must be in full or by reference. */
  if (node->content)
    SVN_ERR_ASSERT_NO_RETURN(node->content
                             && ((SVN_IS_VALID_REVNUM(node->content->ref.rev)
                                  && node->content->ref.relpath)
                                 || (node->content->kind != svn_node_unknown
                                     && node->content->kind != svn_node_none)));
}

svn_branch_el_rev_content_t *
svn_branch_map_get(const svn_branch_instance_t *branch,
                   int eid)
{
  svn_branch_el_rev_content_t *node;

  SVN_ERR_ASSERT_NO_RETURN(BRANCH_FAMILY_HAS_ELEMENT(branch, eid));

  node = svn_int_hash_get(branch->e_map, eid);

  if (node)
    branch_map_node_validate(branch, eid, node);
  return node;
}

/* In BRANCH, set element EID's node (parent, name, content) to NODE.
 *
 * If NODE is null, delete element EID. Otherwise, NODE->content may be
 * null meaning it is unknown.
 *
 * Assume NODE is already allocated with sufficient lifetime.
 */
static void
branch_map_set(svn_branch_instance_t *branch,
               int eid,
               svn_branch_el_rev_content_t *node)
{
  apr_pool_t *map_pool = apr_hash_pool_get(branch->e_map);

  SVN_ERR_ASSERT_NO_RETURN(BRANCH_FAMILY_HAS_ELEMENT(branch, eid));
  if (node)
    branch_map_node_validate(branch, eid, node);

  svn_int_hash_set(branch->e_map, eid, node);
  assert_branch_instance_invariants(branch, map_pool);
}

void
svn_branch_map_delete(svn_branch_instance_t *branch,
                      int eid)
{
  SVN_ERR_ASSERT_NO_RETURN(BRANCH_FAMILY_HAS_ELEMENT(branch, eid));

  branch_map_set(branch, eid, NULL);
}

void
svn_branch_map_update(svn_branch_instance_t *branch,
                      int eid,
                      svn_branch_eid_t new_parent_eid,
                      const char *new_name,
                      const svn_element_content_t *new_content)
{
  apr_pool_t *map_pool = apr_hash_pool_get(branch->e_map);
  svn_branch_el_rev_content_t *node
    = svn_branch_el_rev_content_create(new_parent_eid, new_name, new_content,
                                       map_pool);

  /* EID must be a valid element id */
  SVN_ERR_ASSERT_NO_RETURN(BRANCH_FAMILY_HAS_ELEMENT(branch, eid));
  /* NEW_CONTENT must be specified, either in full or by reference */
  SVN_ERR_ASSERT_NO_RETURN(new_content);

  /* We don't expect to be called more than once per eid. */
  /*SVN_ERR_ASSERT_NO_RETURN(branch_map_get(branch, eid) == NULL); ### hmm, no! */

  /* Insert the new version */
  branch_map_set(branch, eid, node);
}

void
svn_branch_map_update_as_subbranch_root(svn_branch_instance_t *branch,
                                        int eid,
                                        svn_branch_eid_t new_parent_eid,
                                        const char *new_name)
{
  apr_pool_t *map_pool = apr_hash_pool_get(branch->e_map);
  svn_branch_el_rev_content_t *node
    = svn_branch_el_rev_content_create(new_parent_eid, new_name, NULL /*content*/,
                                       map_pool);

  /* EID must be a valid element id */
  SVN_ERR_ASSERT_NO_RETURN(BRANCH_FAMILY_HAS_ELEMENT(branch, eid));
  branch_map_node_validate(branch, eid, node);

  /* We don't expect to be called more than once per eid. */
  /*SVN_ERR_ASSERT_NO_RETURN(branch_map_get(branch, eid) == NULL); ### hmm, no! */

  /* Insert the new version */
  branch_map_set(branch, eid, node);
}

svn_branch_subtree_t
svn_branch_map_get_subtree(const svn_branch_instance_t *branch,
                           int eid,
                           apr_pool_t *result_pool)
{
  svn_branch_subtree_t new_subtree;

  SVN_BRANCH_SEQUENCE_POINT(branch);

  new_subtree.e_map = apr_hash_copy(result_pool, branch->e_map);
  new_subtree.root_eid = eid;
  return new_subtree;
}

static void
map_purge_orphans(apr_hash_t *e_map,
                  int root_eid,
                  apr_pool_t *scratch_pool)
{
  apr_hash_index_t *hi;
  svn_boolean_t changed;

  do
    {
      changed = FALSE;

      for (hi = apr_hash_first(scratch_pool, e_map);
           hi; hi = apr_hash_next(hi))
        {
          int this_eid = svn_int_hash_this_key(hi);
          svn_branch_el_rev_content_t *this_node = apr_hash_this_val(hi);

          if (this_eid != root_eid)
            {
              svn_branch_el_rev_content_t *parent_node
                = svn_int_hash_get(e_map, this_node->parent_eid);

              /* Purge if parent is deleted */
              if (! parent_node)
                {
                  SVN_DBG(("purge orphan: e%d", this_eid));
                  svn_int_hash_set(e_map, this_eid, NULL);
                  changed = TRUE;
                }
              else
                SVN_ERR_ASSERT_NO_RETURN(parent_node->content);
            }
        }
    }
  while (changed);
}

void
svn_branch_map_purge_orphans(svn_branch_instance_t *branch,
                             apr_pool_t *scratch_pool)
{
  map_purge_orphans(branch->e_map, branch->sibling_defn->root_eid, scratch_pool);
}

void
svn_branch_purge_r(svn_branch_instance_t *branch,
                   apr_pool_t *scratch_pool)
{
  SVN_ITER_T(svn_branch_instance_t) *bi;

  /* first, remove elements that have no parent element */
  svn_branch_map_purge_orphans(branch, scratch_pool);

  /* second, remove subbranches that have no subbranch-root element */
  for (SVN_ARRAY_ITER(bi, svn_branch_get_all_sub_branches(
                            branch, scratch_pool, scratch_pool), scratch_pool))
    {
      svn_branch_instance_t *b = bi->val;

      if (svn_branch_map_get(branch, b->outer_eid))
        {
          svn_branch_purge_r(b, bi->iterpool);
        }
      else
        {
          svn_branch_delete_branch_instance_r(b, bi->iterpool);
        }
    }
}

const char *
svn_branch_get_root_rrpath(const svn_branch_instance_t *branch,
                           apr_pool_t *result_pool)
{
  const char *root_rrpath;

  if (branch->outer_branch)
    {
      root_rrpath
        = svn_branch_get_rrpath_by_eid(branch->outer_branch, branch->outer_eid,
                                       result_pool);
    }
  else
    {
      root_rrpath = "";
    }

  SVN_ERR_ASSERT_NO_RETURN(root_rrpath);
  return root_rrpath;
}

const char *
svn_branch_get_path_by_eid(const svn_branch_instance_t *branch,
                           int eid,
                           apr_pool_t *result_pool)
{
  const char *path = "";
  svn_branch_el_rev_content_t *node;

  SVN_ERR_ASSERT_NO_RETURN(BRANCH_FAMILY_HAS_ELEMENT(branch, eid));

  for (; ! IS_BRANCH_ROOT_EID(branch, eid); eid = node->parent_eid)
    {
      node = svn_branch_map_get(branch, eid);
      if (! node)
        return NULL;
      path = svn_relpath_join(node->name, path, result_pool);
    }
  SVN_ERR_ASSERT_NO_RETURN(IS_BRANCH_ROOT_EID(branch, eid));
  return path;
}

const char *
svn_branch_get_rrpath_by_eid(const svn_branch_instance_t *branch,
                             int eid,
                             apr_pool_t *result_pool)
{
  const char *path = svn_branch_get_path_by_eid(branch, eid, result_pool);
  const char *rrpath = NULL;

  if (path)
    {
      rrpath = svn_relpath_join(svn_branch_get_root_rrpath(branch, result_pool),
                                path, result_pool);
    }
  return rrpath;
}

int
svn_branch_get_eid_by_path(const svn_branch_instance_t *branch,
                           const char *path,
                           apr_pool_t *scratch_pool)
{
  apr_hash_index_t *hi;

  /* ### This is a crude, linear search */
  for (hi = apr_hash_first(scratch_pool, branch->e_map);
       hi; hi = apr_hash_next(hi))
    {
      int eid = svn_int_hash_this_key(hi);
      const char *this_path = svn_branch_get_path_by_eid(branch, eid,
                                                         scratch_pool);

      if (! this_path)
        {
          /* Mapping is not complete; this element is in effect not present. */
          continue;
        }
      if (strcmp(path, this_path) == 0)
        {
          return eid;
        }
    }

  return -1;
}

int
svn_branch_get_eid_by_rrpath(svn_branch_instance_t *branch,
                             const char *rrpath,
                             apr_pool_t *scratch_pool)
{
  const char *path = svn_relpath_skip_ancestor(svn_branch_get_root_rrpath(
                                                 branch, scratch_pool),
                                               rrpath);
  int eid = -1;

  if (path)
    {
      eid = svn_branch_get_eid_by_path(branch, path, scratch_pool);
    }
  return eid;
}

svn_error_t *
svn_branch_map_add_subtree(svn_branch_instance_t *to_branch,
                           int to_eid,
                           svn_branch_eid_t new_parent_eid,
                           const char *new_name,
                           svn_branch_subtree_t new_subtree,
                           apr_pool_t *scratch_pool)
{
  apr_hash_index_t *hi;
  svn_branch_el_rev_content_t *new_root_content;

  /* Get a new EID for the root element, if not given. */
  if (to_eid == -1)
    {
      /* Assign a new EID for the new subtree's root element */
      to_eid = svn_branch_family_add_new_element(to_branch->sibling_defn->family);
    }

  /* Create the new subtree root element */
  new_root_content = svn_int_hash_get(new_subtree.e_map, new_subtree.root_eid);
  if (new_root_content->content)
    svn_branch_map_update(to_branch, to_eid,
                          new_parent_eid, new_name, new_root_content->content);
  else
    svn_branch_map_update_as_subbranch_root(to_branch, to_eid,
                                            new_parent_eid, new_name);

  /* Process its immediate children */
  for (hi = apr_hash_first(scratch_pool, new_subtree.e_map);
       hi; hi = apr_hash_next(hi))
    {
      int this_from_eid = svn_int_hash_this_key(hi);
      svn_branch_el_rev_content_t *from_node = apr_hash_this_val(hi);

      if (from_node->parent_eid == new_subtree.root_eid)
        {
          svn_branch_subtree_t this_subtree;

          /* Recurse. (We don't try to check whether it's a directory node,
             as we might not have the node kind in the map.) */
          this_subtree.e_map = new_subtree.e_map;
          this_subtree.root_eid = this_from_eid;
          SVN_ERR(svn_branch_map_add_subtree(to_branch, -1 /*to_eid*/,
                                             to_eid, from_node->name,
                                             this_subtree, scratch_pool));
        }
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_branch_instantiate_subtree(svn_branch_instance_t *to_branch,
                               svn_branch_eid_t new_parent_eid,
                               const char *new_name,
                               svn_branch_subtree_t new_subtree,
                               apr_pool_t *scratch_pool)
{
  apr_hash_index_t *hi;
  svn_branch_el_rev_content_t *new_root_content;

  /* Instantiate the root element of NEW_SUBTREE */
  new_root_content = svn_int_hash_get(new_subtree.e_map, new_subtree.root_eid);
  if (new_root_content->content)
    svn_branch_map_update(to_branch, new_subtree.root_eid,
                          new_parent_eid, new_name, new_root_content->content);
  else
    svn_branch_map_update_as_subbranch_root(to_branch, new_subtree.root_eid,
                                            new_parent_eid, new_name);

  /* Instantiate all the children of NEW_SUBTREE */
  /* ### Writes to NEW_SUBTREE.e_map. No semantic change; just purges orphan
     elements. Could easily avoid this by duplicating first. */
  map_purge_orphans(new_subtree.e_map, new_subtree.root_eid, scratch_pool);
  for (hi = apr_hash_first(scratch_pool, new_subtree.e_map);
       hi; hi = apr_hash_next(hi))
    {
      int this_eid = svn_int_hash_this_key(hi);
      svn_branch_el_rev_content_t *this_node = apr_hash_this_val(hi);

      if (this_eid != new_subtree.root_eid)
        {
          branch_map_set(to_branch, this_eid, this_node);
        }
    }

  return SVN_NO_ERROR;
}

apr_array_header_t *
svn_branch_get_subbranches(const svn_branch_instance_t *branch,
                           int eid,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool)
{
  const char *top_rrpath = svn_branch_get_rrpath_by_eid(branch, eid,
                                                        scratch_pool);
  svn_array_t *subbranches = svn_array_make(result_pool);
  SVN_ITER_T(svn_branch_instance_t) *bi;

  for (SVN_ARRAY_ITER(bi, branch->rev_root->branch_instances, scratch_pool))
    {
      svn_branch_instance_t *subbranch = bi->val;
      const char *sub_branch_root_rrpath
        = svn_branch_get_root_rrpath(subbranch, bi->iterpool);

      /* Is it an immediate child at or below EID? */
      if (subbranch->outer_branch == branch
          && svn_relpath_skip_ancestor(top_rrpath, sub_branch_root_rrpath))
        SVN_ARRAY_PUSH(subbranches) = bi->val;
    }
  return subbranches;
}

apr_array_header_t *
svn_branch_get_all_sub_branches(const svn_branch_instance_t *branch,
                                apr_pool_t *result_pool,
                                apr_pool_t *scratch_pool)
{
  svn_array_t *subbranches = svn_array_make(result_pool);
  SVN_ITER_T(svn_branch_instance_t) *bi;

  for (SVN_ARRAY_ITER(bi, branch->rev_root->branch_instances, scratch_pool))
    {
      /* Is it an immediate child? */
      if (bi->val->outer_branch == branch)
        SVN_ARRAY_PUSH(subbranches) = bi->val;
    }
  return subbranches;
}

svn_branch_instance_t *
svn_branch_get_subbranch_at_eid(svn_branch_instance_t *branch,
                                int eid,
                                apr_pool_t *scratch_pool)
{
  SVN_ITER_T(svn_branch_instance_t) *bi;

  /* TODO: more efficient to search in branch->rev_root->branch_instances */
  for (SVN_ARRAY_ITER(bi, svn_branch_get_all_sub_branches(
                            branch, scratch_pool, scratch_pool), scratch_pool))
    {
      if (bi->val->outer_eid == eid)
        return bi->val;
    }
  return NULL;
}

svn_branch_instance_t *
svn_branch_add_new_branch_instance(svn_branch_instance_t *outer_branch,
                                   int outer_eid,
                                   svn_branch_sibling_t *branch_sibling,
                                   apr_pool_t *scratch_pool)
{
  svn_branch_family_t *family = branch_sibling->family;

  svn_branch_instance_t *branch_instance
    = svn_branch_instance_create(branch_sibling, outer_branch->rev_root,
                                 outer_branch, outer_eid, family->pool);

  SVN_ARRAY_PUSH(branch_instance->rev_root->branch_instances) = branch_instance;

  return branch_instance;
}

/* Remove branch-instance BRANCH from the list of branches in REV_ROOT.
 */
static void
svn_branch_revision_root_delete_branch_instance(
                                svn_branch_revision_root_t *rev_root,
                                svn_branch_instance_t *branch,
                                apr_pool_t *scratch_pool)
{
  SVN_ITER_T(svn_branch_instance_t) *bi;

  SVN_ERR_ASSERT_NO_RETURN(branch->rev_root == rev_root);

  for (SVN_ARRAY_ITER(bi, rev_root->branch_instances, scratch_pool))
    {
      if (bi->val == branch)
        {
          SVN_DBG(("deleting branch-instance b%d e%d",
                   bi->val->sibling_defn->bsid, bi->val->sibling_defn->root_eid));
          svn_sort__array_delete(rev_root->branch_instances, bi->i, 1);
          break;
        }
    }
}

void
svn_branch_delete_branch_instance_r(svn_branch_instance_t *branch,
                                    apr_pool_t *scratch_pool)
{
  SVN_ITER_T(svn_branch_instance_t) *bi;

  for (SVN_ARRAY_ITER(bi, svn_branch_get_all_sub_branches(
                            branch, scratch_pool, scratch_pool), scratch_pool))
    {
      svn_branch_delete_branch_instance_r(bi->val, bi->iterpool);
    }

  svn_branch_revision_root_delete_branch_instance(branch->outer_branch->rev_root,
                                                  branch, scratch_pool);
}


/*
 * ========================================================================
 * Parsing and Serializing
 * ========================================================================
 */

/*  */
static svn_error_t *
parse_branch_line(int *bsid_p,
                  int *root_eid_p,
                  const char **path_p,
                  svn_stream_t *stream,
                  apr_pool_t *scratch_pool)
{
  svn_stringbuf_t *line;
  svn_boolean_t eof;
  int n;
  int offset;

  /* Read a line */
  SVN_ERR(svn_stream_readline(stream, &line, "\n", &eof, scratch_pool));
  SVN_ERR_ASSERT(!eof);

  n = sscanf(line->data, "b%d: root-eid %d at%n",
             bsid_p, root_eid_p, &offset);
  SVN_ERR_ASSERT(n >= 2);  /* C std is unclear on whether '%n' counts */
  SVN_ERR_ASSERT(line->data[offset] == ' ');
  *path_p = line->data + offset + 1;

  if (strcmp(*path_p, ".") == 0)
    *path_p = "";

  return SVN_NO_ERROR;
}

/*  */
static svn_error_t *
parse_element_line(int *bsid_p,
                   int *eid_p,
                   int *parent_eid_p,
                   const char **name_p,
                   svn_stream_t *stream,
                   apr_pool_t *scratch_pool)
{
  svn_stringbuf_t *line;
  svn_boolean_t eof;
  int n;
  int offset;

  /* Read a line */
  SVN_ERR(svn_stream_readline(stream, &line, "\n", &eof, scratch_pool));
  SVN_ERR_ASSERT(!eof);

  n = sscanf(line->data, "b%de%d: %d%n",
             bsid_p, eid_p,
             parent_eid_p, &offset);
  SVN_ERR_ASSERT(n >= 3);  /* C std is unclear on whether '%n' counts */
  SVN_ERR_ASSERT(line->data[offset] == ' ');
  *name_p = line->data + offset + 1;

  if (strcmp(*name_p, "(null)") == 0)
    *name_p = NULL;
  else if (strcmp(*name_p, ".") == 0)
    *name_p = "";

  return SVN_NO_ERROR;
}

/* Create a new branch *NEW_BRANCH that belongs to FAMILY, initialized
 * with info parsed from STREAM, allocated in RESULT_POOL.
 */
static svn_error_t *
svn_branch_instance_parse(svn_branch_instance_t **new_branch,
                          svn_branch_family_t *family,
                          svn_branch_revision_root_t *rev_root,
                          svn_stream_t *stream,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool)
{
  int bsid, root_eid;
  svn_branch_sibling_t *branch_sibling;
  svn_branch_instance_t *branch_instance;
  const char *branch_root_rrpath;
  svn_branch_instance_t *outer_branch;
  int outer_eid;
  int eid;

  SVN_ERR(parse_branch_line(&bsid, &root_eid, &branch_root_rrpath,
                            stream, scratch_pool));

  branch_sibling = family_find_or_create_branch_sibling(family, bsid, root_eid);
  if (branch_root_rrpath[0])
    {
      svn_branch_find_nested_branch_element_by_rrpath(&outer_branch, &outer_eid,
                                                      rev_root->root_branch,
                                                      branch_root_rrpath,
                                                      scratch_pool);
    }
  else
    {
      outer_branch = NULL;
      outer_eid = -1;
    }
  branch_instance = svn_branch_instance_create(branch_sibling, rev_root,
                                               outer_branch, outer_eid,
                                               result_pool);

  /* Read in the structure, leaving the content of each element as null */
  for (eid = family->first_eid; eid < family->next_eid; eid++)
    {
      int this_bsid, this_eid, this_parent_eid;
      const char *this_name;

      SVN_ERR(parse_element_line(&this_bsid, &this_eid,
                                 &this_parent_eid, &this_name,
                                 stream, scratch_pool));

      if (this_name)
        {
          svn_branch_el_rev_content_t *node
            = svn_branch_el_rev_content_create(this_parent_eid, this_name,
                                               NULL /*content*/, result_pool);

          branch_map_set(branch_instance, this_eid, node);
        }
    }

  /* Populate the content reference for each element, now that we have
     enough info to calculate full paths */
  for (eid = family->first_eid; eid < family->next_eid; eid++)
    {
      svn_branch_el_rev_content_t *node
        = svn_branch_map_get(branch_instance, eid);

      if (node)
        {
          const char *rrpath = svn_branch_get_rrpath_by_eid(branch_instance,
                                                            eid, scratch_pool);
          svn_pathrev_t peg;
          svn_element_content_t *content;

          /* Specify the content by reference */
          peg.rev = rev_root->rev;
          peg.relpath = rrpath;
          content = svn_element_content_create_ref(peg, scratch_pool);

          svn_branch_map_update(branch_instance, eid,
                                node->parent_eid, node->name, content);
        }
    }

  *new_branch = branch_instance;
  return SVN_NO_ERROR;
}

/* Parse a branch family from STREAM.
 *
 * If the family is already found in REPOS, update it (assume it's from a
 * later revision), otherwise create a new one and register it in REPOS.
 *
 * Set *NEW_FAMILY to the branch family object, allocated in REPOS's pool.
 */
static svn_error_t *
svn_branch_family_parse(svn_branch_family_t **new_family,
                        int *num_branch_instances,
                        svn_branch_repos_t *repos,
                        svn_stream_t *stream,
                        apr_pool_t *scratch_pool)
{
  svn_stringbuf_t *line;
  svn_boolean_t eof;
  int n;
  int first_bsid, next_bsid, first_eid, next_eid;
  svn_branch_family_t *family;

  SVN_ERR(svn_stream_readline(stream, &line, "\n", &eof, scratch_pool));
  SVN_ERR_ASSERT(!eof);
  n = sscanf(line->data, "family: bsids %d %d eids %d %d "
                         "b-instances %d",
             &first_bsid, &next_bsid, &first_eid, &next_eid,
             num_branch_instances);
  SVN_ERR_ASSERT(n == 5);

  family = repos->family;
  if (family)
    {
      SVN_ERR_ASSERT(first_bsid == family->first_bsid);
      SVN_ERR_ASSERT(next_bsid >= family->next_bsid);
      SVN_ERR_ASSERT(first_eid == family->first_eid);
      SVN_ERR_ASSERT(next_eid >= family->next_eid);
      family->next_bsid = next_bsid;
      family->next_eid = next_eid;
    }
  else
    {
      family = svn_branch_family_create(repos,
                                        first_bsid, next_bsid,
                                        first_eid, next_eid,
                                        repos->pool);

      /* Register this family in the repos */
      repos->family = family;
    }

  *new_family = family;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_branch_revision_root_parse(svn_branch_revision_root_t **rev_root_p,
                               svn_branch_repos_t *repos,
                               svn_stream_t *stream,
                               apr_pool_t *result_pool,
                               apr_pool_t *scratch_pool)
{
  svn_branch_revision_root_t *rev_root;
  svn_revnum_t rev;
  svn_stringbuf_t *line;
  svn_boolean_t eof;
  int n;

  SVN_ERR(svn_stream_readline(stream, &line, "\n", &eof, scratch_pool));
  SVN_ERR_ASSERT(! eof);
  n = sscanf(line->data, "r%ld:",
             &rev);
  SVN_ERR_ASSERT(n == 1);

  rev_root = svn_branch_revision_root_create(repos, rev, NULL /*root_branch*/,
                                             result_pool);

  /* parse the family */
    {
      svn_branch_family_t *family;
      int num_branch_instances;
      int j;

      SVN_ERR(svn_branch_family_parse(&family,
                                      &num_branch_instances,
                                      repos, stream,
                                      scratch_pool));

      /* parse the branches */
      for (j = 0; j < num_branch_instances; j++)
        {
          svn_branch_instance_t *branch;

          SVN_ERR(svn_branch_instance_parse(&branch, family, rev_root, stream,
                                            family->pool, scratch_pool));
          SVN_ARRAY_PUSH(rev_root->branch_instances) = branch;

          /* Note the revision-root branch */
          if (! branch->outer_branch)
            {
              rev_root->root_branch = branch;
            }
        }
    }

  *rev_root_p = rev_root;
  return SVN_NO_ERROR;
}

/* Write to STREAM a parseable representation of BRANCH.
 */
static svn_error_t *
svn_branch_instance_serialize(svn_stream_t *stream,
                              svn_branch_instance_t *branch,
                              apr_pool_t *scratch_pool)
{
  svn_branch_family_t *family = branch->sibling_defn->family;
  const char *branch_root_rrpath = svn_branch_get_root_rrpath(branch,
                                                              scratch_pool);
  int eid;

  SVN_ERR(svn_stream_printf(stream, scratch_pool,
                            "b%d: root-eid %d at %s\n",
                            branch->sibling_defn->bsid,
                            branch->sibling_defn->root_eid,
                            branch_root_rrpath[0] ? branch_root_rrpath : "."));

  svn_branch_map_purge_orphans(branch, scratch_pool);
  for (eid = family->first_eid; eid < family->next_eid; eid++)
    {
      svn_branch_el_rev_content_t *node = svn_branch_map_get(branch, eid);
      int parent_eid;
      const char *name;

      if (node)
        {
          parent_eid = node->parent_eid;
          name = node->name[0] ? node->name : ".";
        }
      else
        {
          /* ### TODO: instead, omit the line completely; but the
                 parser currently can't handle that. */
          parent_eid = -1;
          name = "(null)";
        }
      SVN_ERR(svn_stream_printf(stream, scratch_pool,
                                "b%de%d: %d %s\n",
                                branch->sibling_defn->bsid, eid,
                                parent_eid, name));
    }
  return SVN_NO_ERROR;
}

/* Write to STREAM a parseable representation of FAMILY.
 */
static svn_error_t *
svn_branch_family_serialize(svn_stream_t *stream,
                            svn_branch_revision_root_t *rev_root,
                            svn_branch_family_t *family,
                            apr_pool_t *scratch_pool)
{
  svn_array_t *branch_instances = svn_array_make(scratch_pool);
  SVN_ITER_T(svn_branch_instance_t) *bi;

  for (SVN_ARRAY_ITER(bi, rev_root->branch_instances, scratch_pool))
    if (bi->val->sibling_defn->family == family)
      SVN_ARRAY_PUSH(branch_instances) = bi->val;

  SVN_ERR(svn_stream_printf(stream, scratch_pool,
                            "family: bsids %d %d eids %d %d "
                            "b-instances %d\n",
                            family->first_bsid, family->next_bsid,
                            family->first_eid, family->next_eid,
                            branch_instances->nelts));

  for (SVN_ARRAY_ITER(bi, branch_instances, scratch_pool))
    SVN_ERR(svn_branch_instance_serialize(stream, bi->val, bi->iterpool));
  return SVN_NO_ERROR;
}

svn_error_t *
svn_branch_revision_root_serialize(svn_stream_t *stream,
                                   svn_branch_revision_root_t *rev_root,
                                   apr_pool_t *scratch_pool)
{
  SVN_ERR(svn_stream_printf(stream, scratch_pool,
                            "r%ld:\n",
                            rev_root->rev));

  SVN_ERR(svn_branch_family_serialize(
            stream, rev_root, rev_root->root_branch->sibling_defn->family,
            scratch_pool));

  return SVN_NO_ERROR;
}


/*
 * ========================================================================
 */

void
svn_branch_find_nested_branch_element_by_rrpath(
                                svn_branch_instance_t **branch_p,
                                int *eid_p,
                                svn_branch_instance_t *root_branch,
                                const char *rrpath,
                                apr_pool_t *scratch_pool)
{
  const char *branch_root_path = svn_branch_get_root_rrpath(root_branch,
                                                            scratch_pool);
  SVN_ITER_T(svn_branch_instance_t) *bi;

  if (! svn_relpath_skip_ancestor(branch_root_path, rrpath))
    {
      /* The path we're looking for is not (path-wise) in this branch. */
      *branch_p = NULL;
      if (eid_p)
        *eid_p = -1;
      return;
    }

  /* The path we're looking for is (path-wise) in this branch. See if it
     is also in a sub-branch (recursively). */
  for (SVN_ARRAY_ITER(bi, svn_branch_get_all_sub_branches(
                            root_branch, scratch_pool, scratch_pool),
                      scratch_pool))
    {
      svn_branch_instance_t *sub_branch;
      int sub_branch_eid;

      svn_branch_find_nested_branch_element_by_rrpath(&sub_branch,
                                                      &sub_branch_eid,
                                                      bi->val, rrpath,
                                                      bi->iterpool);
      if (sub_branch)
        {
           *branch_p = sub_branch;
           if (eid_p)
             *eid_p = sub_branch_eid;
           return;
         }
    }

  *branch_p = root_branch;
  if (eid_p)
    *eid_p = svn_branch_get_eid_by_rrpath(root_branch, rrpath, scratch_pool);
}

svn_error_t *
svn_branch_repos_find_el_rev_by_path_rev(svn_branch_el_rev_id_t **el_rev_p,
                                const char *rrpath,
                                svn_revnum_t revnum,
                                const svn_branch_repos_t *repos,
                                apr_pool_t *result_pool,
                                apr_pool_t *scratch_pool)
{
  svn_branch_el_rev_id_t *el_rev = apr_palloc(result_pool, sizeof(*el_rev));
  const svn_branch_revision_root_t *rev_root;

  if (revnum < 0 || revnum >= repos->rev_roots->nelts)
    return svn_error_createf(SVN_ERR_FS_NO_SUCH_REVISION, NULL,
                             _("No such revision %ld"), revnum);

  rev_root = svn_array_get(repos->rev_roots, (int)(revnum));
  el_rev->rev = revnum;
  svn_branch_find_nested_branch_element_by_rrpath(&el_rev->branch, &el_rev->eid,
                                                  rev_root->root_branch, rrpath,
                                                  scratch_pool);

  /* Any path must at least be within the repository root branch */
  SVN_ERR_ASSERT_NO_RETURN(el_rev->branch);
  *el_rev_p = el_rev;
  return SVN_NO_ERROR;
}


/*
 * ========================================================================
 */

const char *
svn_branch_instance_get_id(svn_branch_instance_t *branch,
                           apr_pool_t *result_pool)
{
  const char *id = "";

  while (branch->outer_branch)
    {
      id = apr_psprintf(result_pool, ".%d%s",
                        branch->outer_eid, id);
      branch = branch->outer_branch;
    }
  id = apr_psprintf(result_pool, "^%s", id);
  return id;
}

svn_error_t *
svn_branch_branch_subtree_r(svn_branch_instance_t **new_branch_p,
                            svn_branch_instance_t *from_branch,
                            int from_eid,
                            svn_branch_instance_t *to_outer_branch,
                            svn_branch_eid_t to_outer_parent_eid,
                            const char *new_name,
                            apr_pool_t *scratch_pool)
{
  int to_outer_eid;
  svn_branch_sibling_t *new_branch_def;

  /* Source element must exist */
  if (! svn_branch_get_path_by_eid(from_branch, from_eid, scratch_pool))
    {
      return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                               _("cannot branch from b%d e%d: "
                                 "does not exist"),
                               from_branch->sibling_defn->bsid, from_eid);
    }

  /* assign new eid to root node (outer branch) */
  to_outer_eid
    = svn_branch_family_add_new_element(to_outer_branch->sibling_defn->family);
  svn_branch_map_update_as_subbranch_root(to_outer_branch, to_outer_eid,
                                          to_outer_parent_eid, new_name);

  /* create new inner branch sibling-defn (for the top-level branching only,
     not for any nested branches, as their sibling-defns already exist) */
  new_branch_def
    = svn_branch_family_add_new_branch_sibling(from_branch->sibling_defn->family,
                                               from_eid);

  SVN_ERR(svn_branch_branch_subtree_r2(new_branch_p,
                                       from_branch, from_eid,
                                       to_outer_branch, to_outer_eid,
                                       new_branch_def,
                                       scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_branch_branch_subtree_r2(svn_branch_instance_t **new_branch_p,
                             svn_branch_instance_t *from_branch,
                             int from_eid,
                             svn_branch_instance_t *to_outer_branch,
                             svn_branch_eid_t to_outer_eid,
                             svn_branch_sibling_t *new_branch_def,
                             apr_pool_t *scratch_pool)
{
  svn_branch_subtree_t from_subtree
    = svn_branch_map_get_subtree(from_branch, from_eid, scratch_pool);
  svn_branch_instance_t *new_branch;

  /* create new inner branch instance */
  new_branch = svn_branch_add_new_branch_instance(to_outer_branch, to_outer_eid,
                                                  new_branch_def, scratch_pool);

  /* Populate the new branch mapping */
  SVN_ERR(svn_branch_instantiate_subtree(new_branch, -1, "", from_subtree,
                                         scratch_pool));

  /* branch any subbranches under FROM_BRANCH:FROM_EID */
  {
    SVN_ITER_T(svn_branch_instance_t) *bi;

    for (SVN_ARRAY_ITER(bi, svn_branch_get_subbranches(
                              from_branch, from_subtree.root_eid,
                              scratch_pool, scratch_pool), scratch_pool))
      {
        svn_branch_instance_t *subbranch = bi->val;

        /* branch this subbranch into NEW_BRANCH (recursing) */
        SVN_ERR(svn_branch_branch_subtree_r2(NULL,
                                             subbranch,
                                             subbranch->sibling_defn->root_eid,
                                             new_branch, subbranch->outer_eid,
                                             subbranch->sibling_defn,
                                             bi->iterpool));
      }
  }

  if (new_branch_p)
    *new_branch_p = new_branch;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_branch_branch(svn_branch_instance_t **new_branch_p,
                  svn_branch_instance_t *from_branch,
                  int from_eid,
                  svn_branch_instance_t *to_outer_branch,
                  svn_branch_eid_t to_outer_parent_eid,
                  const char *new_name,
                  apr_pool_t *scratch_pool)
{
  SVN_ERR(svn_branch_branch_subtree_r(new_branch_p,
                                      from_branch, from_eid,
                                      to_outer_branch, to_outer_parent_eid,
                                      new_name,
                                      scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_branch_branch_into(svn_branch_instance_t *from_branch,
                       int from_eid,
                       svn_branch_instance_t *to_branch,
                       svn_branch_eid_t to_parent_eid,
                       const char *new_name,
                       apr_pool_t *scratch_pool)
{
  svn_branch_subtree_t from_subtree
    = svn_branch_map_get_subtree(from_branch, from_eid, scratch_pool);

  /* Source element must exist */
  if (! svn_branch_get_path_by_eid(from_branch, from_eid, scratch_pool))
    {
      return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                               _("cannot branch from b%d e%d: "
                                 "does not exist"),
                               from_branch->sibling_defn->bsid, from_eid);
    }

  /* Populate the new branch mapping */
  SVN_ERR(svn_branch_instantiate_subtree(to_branch, to_parent_eid, new_name,
                                         from_subtree, scratch_pool));

  /* branch any subbranches under FROM_BRANCH:FROM_EID */
  {
    SVN_ITER_T(svn_branch_instance_t) *bi;

    for (SVN_ARRAY_ITER(bi, svn_branch_get_subbranches(
                              from_branch, from_subtree.root_eid,
                              scratch_pool, scratch_pool), scratch_pool))
      {
        svn_branch_instance_t *subbranch = bi->val;

        /* branch this subbranch into NEW_BRANCH (recursing) */
        SVN_ERR(svn_branch_branch_subtree_r2(NULL,
                                             subbranch,
                                             subbranch->sibling_defn->root_eid,
                                             to_branch, subbranch->outer_eid,
                                             subbranch->sibling_defn,
                                             bi->iterpool));
      }
  }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_branch_copy_subtree_r(const svn_branch_el_rev_id_t *from_el_rev,
                          svn_branch_instance_t *to_branch,
                          svn_branch_eid_t to_parent_eid,
                          const char *to_name,
                          apr_pool_t *scratch_pool)
{
  SVN_DBG(("cp subtree from e%d to e%d/%s",
           from_el_rev->eid, to_parent_eid, to_name));

  /* copy the subtree, assigning new EIDs */
  SVN_ERR(svn_branch_map_add_subtree(to_branch, -1 /*to_eid*/,
                                     to_parent_eid, to_name,
                                     svn_branch_map_get_subtree(
                                       from_el_rev->branch, from_el_rev->eid,
                                       scratch_pool),
                                     scratch_pool));

  /* handle any subbranches under FROM_BRANCH:FROM_EID */
  /* ### Later. */

  return SVN_NO_ERROR;
}

