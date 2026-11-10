"""Construct a recovery tree from actual image contents, rejecting nonlaminar input."""
import copy

def from_manifests(a):
    scopes=[frozenset(s) for s in a['scopes']];atoms=set(range(len(a['benefit'])))
    if len(scopes)!=len(a['menus']):raise ValueError('One scope is required for every image menu')
    if any(not s<=atoms for s in scopes):raise ValueError('Unknown atomic fragment')
    if any(s&t and not(s<=t or t<=s) for i,s in enumerate(scopes) for t in scopes[i+1:]):raise ValueError('Nonlaminar manifests require the scope-incidence solver')
    # Nonempty scopes, with equal sets forming a stable chain of independent choices.
    order=sorted(range(len(scopes)),key=lambda i:(-len(scopes[i]),i));parent=[-1];benefit=[0];menus=[[{'cost':0,'replicas':[]}]];image_nodes={}
    for image in order:
        candidates=[(len(scopes[j]),-node,node) for j,node in image_nodes.items() if scopes[image]<=scopes[j]]
        p=min(candidates)[2] if candidates else 0;node=len(parent);image_nodes[image]=node
        parent.append(p);benefit.append(0);menus.append(copy.deepcopy(a['menus'][image]))
    atom_nodes={}
    for atom,value in enumerate(a['benefit']):
        candidates=[(len(scopes[i]),-node,node) for i,node in image_nodes.items() if atom in scopes[i]]
        p=min(candidates)[2] if candidates else 0;atom_nodes[atom]=len(parent);parent.append(p);benefit.append(value);menus.append([{'cost':0,'replicas':[]}])
    actual=[set() for _ in parent]
    for atom,node in atom_nodes.items():
        v=node
        while v>=0:actual[v].add(atom);v=parent[v]
    for image,node in image_nodes.items():
        if actual[node]!=set(scopes[image]):raise AssertionError('Constructed tree changes an image manifest')
    result={k:copy.deepcopy(a[k]) for k in ('domain_parent','events','budget')};result.update(parent=parent,benefit=benefit,menus=menus)
    return result,{'image_nodes':image_nodes,'atom_nodes':atom_nodes,'contents_verified':True}
