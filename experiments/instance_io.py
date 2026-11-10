"""Portable integer input format and deterministic independent test generators."""
from __future__ import annotations
import hashlib
import json
import math
import pathlib
import random

def hierarchy(leaves, topology, rng):
    if leaves < 2:
        return [-1]
    if topology == "star":
        return [-1] + [0] * leaves
    if topology == "balanced":
        p = [-1]
        def split(node, count):
            if count == 1:
                return
            left, right = len(p), len(p) + 1
            p.extend([node, node])
            split(left, count // 2)
            split(right, count - count // 2)
        split(0, leaves)
        return p
    if topology == "random":
        p = [-1] * leaves
        active = list(range(leaves))
        while len(active) > 1:
            selected = rng.sample(active, min(len(active), rng.randint(2, 4)))
            root = len(p)
            p.append(-1)
            for v in selected:
                p[v] = root
                active.remove(v)
            active.append(root)
        return p
    raise ValueError(topology)

def leaf_masks(parent):
    children = [[] for _ in parent]
    for v, p in enumerate(parent):
        if p >= 0:
            children[p].append(v)
    leaves = [v for v, row in enumerate(children) if not row]
    masks = [0] * len(parent)
    for i, leaf in enumerate(leaves):
        v = leaf
        while v >= 0:
            masks[v] |= 1 << i
            v = parent[v]
    return masks, leaves

def generate(*, leaves=64, nodes=32, budget=32, replicas=2, options=5,
             events=128, topology="balanced", seed=1, duplicate_fraction=0,
             higher_order=False, test_boundaries=False):
    rng = random.Random(seed)
    dp = hierarchy(leaves, topology, rng)
    _, leaf_ids = leaf_masks(dp)
    parent = [-1] + [rng.randrange(v) for v in range(1, nodes)]
    benefits = [rng.randint(1, 1000) for _ in range(nodes)]
    payload = [rng.randint(1, 32) for _ in range(nodes)]
    subtree = payload.copy()
    for v in reversed(range(1, nodes)):
        subtree[parent[v]] += subtree[v]
    menus = []
    for v in range(nodes):
        row = [{"cost": 0, "replicas": []}]
        if v != 0 or test_boundaries:
            for _ in range(options):
                chosen = sorted(rng.sample(leaf_ids, min(replicas, len(leaf_ids))))
                cost = 1 + math.ceil(128 * subtree[v] * len(chosen) / subtree[0])
                if test_boundaries:
                    cost = rng.randint(0, max(1, budget + 2))
                if len(row) > 1 and rng.random() < duplicate_fraction:
                    old = rng.choice(row[1:])
                    chosen, cost = old["replicas"].copy(), old["cost"] + rng.randint(0, 3)
                row.append({"cost": cost, "replicas": chosen})
        menus.append(row)
    failure = [{"mass": 1000, "domains": []}]
    for _ in range(events):
        k = rng.choice([1, 2, 3, 4] if higher_order else [1, 2])
        failure.append({"mass": rng.randint(1, 9), "domains": sorted(rng.sample(range(len(dp)), min(k, len(dp))))})
    if test_boundaries:
        benefits[rng.randrange(nodes)] = 0
    return {"domain_parent": dp, "parent": parent, "benefit": benefits,
            "menus": menus, "budget": budget, "events": failure}

def write_input(path, a):
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [f"PFL_R1 1 {len(a['domain_parent'])} {len(a['parent'])} {a['budget']} {len(a['events'])}"]
    for key in ("domain_parent", "parent", "benefit"):
        lines.append(" ".join(map(str, a[key])))
    for menu in a["menus"]:
        lines.append(str(len(menu)))
        for o in menu:
            lines.append(" ".join(map(str, [o["cost"], len(o["replicas"]), *o["replicas"]])))
    for e in a["events"]:
        lines.append(" ".join(map(str, [e["mass"], len(e["domains"]), *e["domains"]])))
    data = ("\n".join(lines) + "\n").encode()
    path.write_bytes(data)
    return hashlib.sha256(data).hexdigest()

def evaluate(a, selected):
    masks, _ = leaf_masks(a["domain_parent"])
    failed = [sum_masks(masks, e["domains"]) for e in a["events"]]
    children = [[] for _ in a["parent"]]
    order = []
    for v, p in enumerate(a["parent"]):
        if p < 0:
            order.append(v)
        else:
            children[p].append(v)
    for v in order:
        order.extend(children[v])
    path = [0] * len(order)
    objective = cost = 0
    for v in order:
        option = a["menus"][v][selected[v]]
        cost += option["cost"]
        path[v] = (path[a["parent"][v]] if a["parent"][v] >= 0 else 0) | sum_masks(masks, option["replicas"])
        objective += a["benefit"][v] * sum(e["mass"] for e, f in zip(a["events"], failed) if path[v] & ~f)
    return objective, cost

def sum_masks(masks, nodes):
    result = 0
    for n in nodes:
        result |= masks[n]
    return result

def enumerate_optimum(a):
    best, best_selected, feasible = -1, None, 0
    selected = [0] * len(a["parent"])
    def visit(v, cost):
        nonlocal best, best_selected, feasible
        if v == len(selected):
            feasible += 1
            value, _ = evaluate(a, selected)
            if value > best:
                best, best_selected = value, selected.copy()
            return
        for i, o in enumerate(a["menus"][v]):
            if cost + o["cost"] <= a["budget"]:
                selected[v] = i
                visit(v + 1, cost + o["cost"])
    visit(0, 0)
    return best, best_selected, feasible

