from pathlib import Path

p = Path("net/tipc/group.c")
s = p.read_text()

old = """\tif (tipc_topsrv_kern_subscr(net, portid, type, 0, ~0, &grp->subid))
\t\tWRITE_ONCE(*grp->open, false);
\t\treturn grp;
\tkfree(grp);
"""
new = """\tif (tipc_topsrv_kern_subscr(net, portid, type, 0, ~0, &grp->subid)) {
\t\tWRITE_ONCE(*grp->open, false);
\t\treturn grp;
\t}
\tkfree(grp);
"""
if s.count(old) != 1:
    raise SystemExit(f"group_create control-flow pattern count={s.count(old)}")
s = s.replace(old, new, 1)

old = """void tipc_group_delete(struct net *net, struct tipc_group *grp)
{
\tWRITE_ONCE(*grp->open, false);
\tstruct rb_root *tree = &grp->members;
\tstruct tipc_member *m, *tmp;
\tstruct sk_buff_head xmitq;

\t__skb_queue_head_init(&xmitq);
"""
new = """void tipc_group_delete(struct net *net, struct tipc_group *grp)
{
\tstruct rb_root *tree = &grp->members;
\tstruct tipc_member *m, *tmp;
\tstruct sk_buff_head xmitq;

\tWRITE_ONCE(*grp->open, false);
\t__skb_queue_head_init(&xmitq);
"""
if s.count(old) != 1:
    raise SystemExit(f"group_delete declaration pattern count={s.count(old)}")
s = s.replace(old, new, 1)

p.write_text(s)

check = p.read_text()
assert "if (tipc_topsrv_kern_subscr(net, portid, type, 0, ~0, &grp->subid)) {" in check
assert "\t\tWRITE_ONCE(*grp->open, false);\n\t\treturn grp;\n\t}\n\tkfree(grp);" in check
assert "{\n\tWRITE_ONCE(*grp->open, false);\n\tstruct rb_root" not in check
print("TIPC post-verify source correction: PASS")
