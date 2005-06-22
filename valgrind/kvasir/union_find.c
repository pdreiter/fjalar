// Implementation of generic union-find data structure
// with union-by-rank and path-compression
// Based on http://www.cs.rutgers.edu/~chvatal/notes/uf.html
// Philip Guo

#include "union_find.h"
#include "tool.h"
#include <limits.h>
#include "dyncomp_runtime.h"

uf_name uf_find(uf_object *object) {
  uf_object *root, *next;

  // Find the root:
  for(root=object; root->parent!=root; root=root->parent);

  // Path-compression:
  for(next=object->parent; next!=root; object=next, next=object->parent) {
    object->parent=root;
    INC_REF_COUNT(root);
    DEC_REF_COUNT(next);
    //    CHECK_REF_COUNT_NULL(next);
  }

  return root;
}

void uf_make_set(uf_object *new_object, unsigned int t) {
  new_object->parent = new_object;
  new_object->rank = 0;
  new_object->tag = t;

#ifdef USE_REF_COUNT
  new_object->ref_count = 0;
#endif
}

// Returns the new leader (uf_name)
// Stupid question: Is there any problems that arise
// when uf_union is called multiple times on the same objects?
// I don't think so, right?
uf_name uf_union(uf_object *obj1, uf_object *obj2) {
  uf_name class1 = uf_find(obj1);
  uf_name class2 = uf_find(obj2);

  // Union-by-rank:

  // If class1 == class2, then obj1 and obj2 are already
  // in the same set so don't do anything! (Is this correct?)
  if (class1 == class2) {
    return class1;
  }

  if(class1->rank < class2->rank) {
    DEC_REF_COUNT(class1->parent);
    //    CHECK_REF_COUNT_NULL(class1->parent);
    class1->parent = class2;
    INC_REF_COUNT(class2);
    return class2;
  }
  else {
    DEC_REF_COUNT(class2->parent);
    //    CHECK_REF_COUNT_NULL(class2->parent);
    class2->parent = class1;
    INC_REF_COUNT(class1);
    if(class1->rank == class2->rank) {
      (class1->rank)++;
    }
    return class1;
  }
}
