use crate::fxhash::HashSet;

pub const OPAQUE_TRIANGLE_ID: u64 = 0x8000_0000_0000_0001;
pub const TABLE_BUFFERS: u64 = 31;

#[derive(Debug, Clone)]
struct IsectFn {
    id: u64,
    symbol: String,
    bounding_box: bool,
    result_ty: String,
    params: Vec<(String, Role)>,
}

#[derive(Debug, Clone, PartialEq)]
enum Role {
    Origin,
    Direction,
    MinDistance,
    MaxDistance,
    PrimitiveId,
    GeometryId,
    InstanceId,
    UserInstanceId,
    WorldOrigin,
    WorldDirection,
    Barycentric,
    FrontFacing,
    Distance,
    /// rtopt4: [[primitive_data]], from the structure's header (nvmtl_rt_primitive_data)
    PrimitiveData,
    Buffer(u64),
}

fn metadata_nodes(module: &str) -> crate::fxhash::HashMap<String, String> {
    module
        .lines()
        .filter_map(|l| {
            let t = l.trim_start();
            if !t.starts_with('!') {
                return None;
            }
            let (k, v) = t.split_once(" = ")?;
            Some((k.trim().to_string(), v.trim().to_string()))
        })
        .collect()
}

fn node_refs(body: &str) -> Vec<String> {
    body.split(|c: char| c == ',' || c == '{' || c == '}' || c.is_whitespace())
        .filter(|t| t.starts_with('!') && t[1..].chars().all(|c| c.is_ascii_digit()) && t.len() > 1)
        .map(str::to_string)
        .collect()
}

fn role_of(node: &str) -> Result<(u32, Role), String> {
    let idx: u32 = node
        .split("i32 ")
        .nth(1)
        .and_then(|s| s.split(|c: char| !c.is_ascii_digit()).next())
        .and_then(|s| s.parse().ok())
        .ok_or_else(|| format!("intersection function argument node has no index: {node}"))?;
    let quoted: Vec<&str> = node
        .split("!\"")
        .skip(1)
        .map(|s| s.split('"').next().unwrap_or(""))
        .collect();
    let role = match quoted.first().copied().unwrap_or("") {
        "air.origin" => Role::Origin,
        "air.direction" => Role::Direction,
        "air.min_distance" => Role::MinDistance,
        "air.max_distance" => Role::MaxDistance,
        "air.primitive_id" => Role::PrimitiveId,
        "air.geometry_id" => Role::GeometryId,
        "air.instance_id" => Role::InstanceId,
        "air.user_instance_id" => Role::UserInstanceId,
        "air.world_space_origin" => Role::WorldOrigin,
        "air.world_space_direction" => Role::WorldDirection,
        "air.barycentric_coord" => Role::Barycentric,
        "air.front_facing" => Role::FrontFacing,
        "air.distance" => Role::Distance,
        "air.primitive_data" => Role::PrimitiveData,
        "air.buffer" => {
            let loc = node
                .split("!\"air.location_index\", i32 ")
                .nth(1)
                .and_then(|s| s.split(|c: char| !c.is_ascii_digit()).next())
                .and_then(|s| s.parse::<u64>().ok())
                .ok_or_else(|| {
                    format!("intersection function buffer argument has no location: {node}")
                })?;
            if loc >= TABLE_BUFFERS {
                return Err(format!("intersection function buffer(%{loc}) is past the table's {TABLE_BUFFERS} buffer slots"));
            }
            Role::Buffer(loc)
        }
        other => {
            return Err(format!(
                "intersection function argument role {other} is not carried yet"
            ))
        }
    };
    Ok((idx, role))
}

fn intersection_functions(
    candidates: &[(String, String)],
    ids: &[u64],
    tags: &str,
) -> Result<Vec<IsectFn>, String> {
    let mut out = Vec::new();
    for (k, (symbol, module)) in candidates.iter().enumerate() {
        let nodes = metadata_nodes(module);
        let Some(list) = nodes.get("!air.intersection") else {
            continue;
        };
        let at = format!("@{symbol},");
        let entry = node_refs(list)
            .into_iter()
            .filter_map(|r| nodes.get(&r).cloned())
            .find(|n| n.contains(&at));
        let Some(entry) = entry else { continue };
        let bounding_box = entry.contains("!\"air.bounding_box\"");
        if !bounding_box && !entry.contains("!\"air.triangle\"") {
            return Err(format!("intersection function {symbol}: only bounding_box and triangle functions are carried"));
        }
        for want in tags.split('.') {
            if !entry.contains(&format!("!\"air.{want}\"")) {
                return Err(format!(
                    "intersection function {symbol} does not declare the query's tag {want}"
                ));
            }
        }
        let def = module
            .lines()
            .find(|l| l.trim_start().starts_with("define ") && l.contains(&format!("@{symbol}(")))
            .ok_or_else(|| {
                format!("intersection function {symbol}: no definition in its module")
            })?;
        let head = def.trim_start().trim_start_matches("define ");
        let at_sym = head.find(&format!("@{symbol}(")).unwrap();
        let ret_part = &head[..at_sym];
        let result_ty = ret_part
            .split_whitespace()
            .filter(|w| {
                !matches!(
                    *w,
                    "internal"
                        | "noundef"
                        | "zeroext"
                        | "signext"
                        | "local_unnamed_addr"
                        | "dso_local"
                        | "fastcc"
                        | "hidden"
                )
            })
            .collect::<Vec<_>>()
            .join(" ");
        let ok_result = if bounding_box {
            result_ty == "<{ i1, float }>" || result_ty == "{ i1, float }"
        } else {
            result_ty == "i1"
        };
        if !ok_result {
            return Err(format!("intersection function {symbol}: result {result_ty} is not carried (bool for triangles, {{bool, float}} for boxes)"));
        }
        let open = head.find('(').unwrap();
        let close = head
            .rfind(')')
            .ok_or("definition without a parameter list")?;
        let param_types: Vec<String> = split_top(&head[open + 1..close])
            .into_iter()
            .map(|p| {
                let toks: Vec<&str> = p.split_whitespace().collect();
                let mut ty = Vec::new();
                for t in toks {
                    if t.starts_with('%')
                        || t.starts_with('"')
                        || matches!(
                            t,
                            "noundef"
                                | "nocapture"
                                | "readonly"
                                | "writeonly"
                                | "nonnull"
                                | "captures(none)"
                        )
                    {
                        break;
                    }
                    ty.push(t);
                }
                ty.join(" ")
            })
            .collect();
        let args_node = node_refs(&entry)
            .get(1)
            .and_then(|r| nodes.get(r))
            .cloned()
            .ok_or_else(|| format!("{symbol}: no argument list node"))?;
        let mut roles = vec![None; param_types.len()];
        for r in node_refs(&args_node) {
            let n = nodes
                .get(&r)
                .ok_or_else(|| format!("{symbol}: argument node {r} missing"))?;
            let (i, role) = role_of(n)?;
            *roles.get_mut(i as usize).ok_or_else(|| {
                format!(
                    "{symbol}: argument {i} beyond its {} parameters",
                    param_types.len()
                )
            })? = Some(role);
        }
        let mut params = Vec::new();
        for (i, (ty, role)) in param_types.into_iter().zip(roles).enumerate() {
            params.push((
                ty,
                role.ok_or_else(|| format!("{symbol}: parameter {i} has no argument role"))?,
            ));
        }
        out.push(IsectFn {
            // the handle value the plugin writes into the table for this candidate: its place plus one, or the
            // process-wide value of a pipeline-independent binary function (MTL4BinaryFunctionOptionPipelineIndependent)
            id: ids.get(k).copied().unwrap_or(k as u64 + 1),
            symbol: symbol.clone(),
            bounding_box,
            result_ty,
            params,
        });
    }
    Ok(out)
}

fn split_top(s: &str) -> Vec<String> {
    let (mut depth, mut cur, mut out) = (0i32, String::new(), Vec::new());
    for c in s.chars() {
        match c {
            '(' | '{' | '<' | '[' => depth += 1,
            ')' | '}' | '>' | ']' => depth -= 1,
            _ => {}
        }
        if c == ',' && depth == 0 {
            out.push(cur.trim().to_string());
            cur.clear();
        } else {
            cur.push(c);
        }
    }
    if !cur.trim().is_empty() {
        out.push(cur.trim().to_string());
    }
    out
}

fn arg_type(a: &str) -> Result<String, String> {
    let a = a.trim();
    let bytes: Vec<char> = a.chars().collect();
    let close_of = |open: char, close: char| -> Option<usize> {
        let mut d = 0i32;
        for (i, c) in bytes.iter().enumerate() {
            if *c == open {
                d += 1;
            }
            if *c == close {
                d -= 1;
                if d == 0 {
                    return Some(i);
                }
            }
        }
        None
    };
    let ty = if a.starts_with("<{") {
        close_of('<', '>').map(|e| a[..=e].to_string())
    } else if a.starts_with('<') {
        close_of('<', '>').map(|e| a[..=e].to_string())
    } else if a.starts_with('{') {
        close_of('{', '}').map(|e| a[..=e].to_string())
    } else if a.starts_with("ptr addrspace(") {
        a.find(')').map(|e| a[..=e].to_string())
    } else {
        a.split_whitespace().next().map(str::to_string)
    };
    ty.ok_or_else(|| format!("intersection tables: operand type of {a} did not parse"))
}

/// The position of the parameter named `value` (`%3`, `%t.coerce`, `%"quoted name"`) in a `define` line.
fn param_position(define: &str, value: &str) -> Option<u32> {
    let at = define.find('@')?;
    let open = at + define[at..].find('(')?;
    let close = define.rfind(')')?;
    split_top(&define[open + 1..close])
        .iter()
        .position(|p| p.split_whitespace().last() == Some(value))
        .and_then(|k| u32::try_from(k).ok())
}

/// The implicit number of an unlabelled entry block: the count of unnamed (numbered) parameters before it.
fn unnamed_param_count(define: &str) -> usize {
    let Some(at) = define.find('@') else { return 0 };
    let (Some(open), Some(close)) = (define[at..].find('(').map(|o| at + o), define.rfind(')')) else { return 0 };
    split_top(&define[open + 1..close])
        .iter()
        .filter(|p| {
            p.split_whitespace()
                .last()
                .and_then(|v| v.strip_prefix('%'))
                .is_some_and(|v| !v.is_empty() && v.chars().all(|c| c.is_ascii_digit()))
        })
        .count()
}

fn defining_line<'a>(entry_body: &'a str, value: &str) -> Option<&'a str> {
    let key = format!("{value} = ");
    entry_body
        .lines()
        .find(|l| l.trim_start().starts_with(&key))
}

pub fn rewrite_runtime_intersection_tables(
    ll: &str,
    entry: &str,
    candidates: &[(String, String)],
) -> Result<String, String> {
    rewrite_intersects(ll, entry, candidates, &[], false)
}

/// The same with each candidate's switch case value given (`ids[k]` for candidate k; a missing one is k + 1).
pub fn rewrite_runtime_intersection_tables_with_ids(
    ll: &str,
    entry: &str,
    candidates: &[(String, String)],
    ids: &[u64],
) -> Result<String, String> {
    rewrite_intersects(ll, entry, candidates, ids, false)
}

/// rtopt4: `air.intersect.*curve_data*` calls without an intersection function table become the same query loop
/// (triangles committed, curve segments intersected by the runtime, user boxes skipped as Metal does without a table);
/// so do the table-less calls whose user instance ID or primitive data is read (both come from the structure's header,
/// which the loop reaches through its query: a structure loaded from an argument buffer has one use, as before).
pub(crate) fn rewrite_curve_intersects(ll: &str, entry: &str) -> Result<String, String> {
    rewrite_intersects(ll, entry, &[], &[], true)
}

/// Splits the block at `lines[i]`: the line is replaced by `br label %{prefix}.entry`, then `inline` (which must end
/// in `br label %{prefix}.cont`), then the `{prefix}.cont:` label that takes the rest of the block. Phis of the block's
/// successors are retargeted from the block's label to `{prefix}.cont`. Returns the number of lines inserted.
pub(crate) fn splice_inline(lines: &mut Vec<String>, i: usize, prefix: &str, inline: &str) -> Result<usize, String> {
    let mut orig = None;
    for j in (1..i).rev() {
        let t = lines[j].trim();
        if let Some((lbl, _)) = t.split_once(':') {
            if !lbl.is_empty() && !lbl.contains(' ') && !t.starts_with(';') && !lbl.starts_with('%') {
                orig = Some(format!("%{lbl}"));
                break;
            }
        }
    }
    let orig = match orig {
        Some(l) => l,
        None => {
            // rtopt5: an unlabelled entry block's number counts the numbered parameters only (named ones take none)
            let head = &lines[0];
            head.find('(').ok_or("entry define without parameters")?;
            head.rfind(')').ok_or("entry define unterminated")?;
            format!("%{}", unnamed_param_count(head))
        }
    };
    let mut succ = Vec::new();
    let mut k = i + 1;
    while k < lines.len() {
        let t = lines[k].trim_start();
        let is_term = t.starts_with("br ")
            || t.starts_with("switch ")
            || t.starts_with("ret ")
            || t == "ret void"
            || t.starts_with("unreachable");
        if is_term {
            let mut term = lines[k].clone();
            if t.starts_with("switch ") {
                let mut m = k + 1;
                while m < lines.len() && !lines[m - 1].contains(']') {
                    term.push_str(&lines[m]);
                    m += 1;
                }
            }
            for part in term.split("label ").skip(1) {
                if let Some(l) = part.split([',', ' ', ']']).next() {
                    succ.push(l.trim().to_string());
                }
            }
            break;
        }
        k += 1;
    }
    let cont = format!("%{prefix}.cont");
    for s in &succ {
        let want = format!("{}:", s.trim_start_matches('%'));
        if let Some(at) = lines.iter().position(|l| {
            l.trim_start().starts_with(&want)
                && (l.trim_start().len() == want.len() || l.trim_start()[want.len()..].starts_with(' '))
        }) {
            let mut m = at + 1;
            while m < lines.len() && lines[m].contains(" = phi ") {
                lines[m] = lines[m].replace(&format!(", {orig} ]"), &format!(", {cont} ]"));
                m += 1;
            }
        }
    }
    // the inline's entry block continues the current block (no branch): a structure loaded in this block (an
    // argument buffer's) stays in the block of the query that uses it, as the native emitter resolves it
    let mut repl: Vec<String> = Vec::new();
    let entry_label = format!("{prefix}.entry:");
    let merged = inline.lines().next().map(str::trim) == Some(entry_label.as_str());
    for (k, l) in inline.lines().enumerate() {
        if k == 0 && merged {
            continue;
        }
        // phis naming the inline's entry name the block it continues
        repl.push(if merged { l.replace(&format!("%{prefix}.entry ]"), &format!("{orig} ]")) } else { l.to_string() });
    }
    if !merged {
        repl.insert(0, format!("  br label %{prefix}.entry"));
    }
    repl.push(format!("{prefix}.cont:"));
    let rl = repl.len();
    lines.splice(i..=i, repl);
    Ok(rl)
}

/// The entry's body as lines, and its byte range in `ll`.
pub(crate) fn entry_body(ll: &str, entry: &str) -> Result<(usize, usize), String> {
    let def_at = ll
        .find(&format!("@{entry}("))
        .and_then(|at| ll[..at].rfind("define "))
        .ok_or_else(|| format!("ray tracing: entry {entry} has no definition"))?;
    let body_end = ll[def_at..]
        .find("\n}\n")
        .map(|e| def_at + e + 3)
        .ok_or("ray tracing: entry body is unterminated")?;
    Ok((def_at, body_end))
}

/// curves = false: the calls with an intersection function table (any tags but motion; curve_data included).
/// curves = true: the curve_data calls without a table.
fn rewrite_intersects(
    ll: &str,
    entry: &str,
    candidates: &[(String, String)],
    ids: &[u64],
    curves: bool,
) -> Result<String, String> {
    let (def_at, body_end) = entry_body(ll, entry)?;
    let mut lines: Vec<String> = ll[def_at..body_end].lines().map(str::to_string).collect();
    let mut declares: Vec<String> = Vec::new();
    let mut table_params: HashSet<u32> = HashSet::default();
    let mut n = 0usize;
    let mut i = 1usize;
    while i < lines.len() {
        let line = lines[i].clone();
        let Some(call_at) = line.find("@air.intersect.") else {
            i += 1;
            continue;
        };
        if !line.contains("call ") {
            i += 1;
            continue;
        }
        let name_end = line[call_at..]
            .find('(')
            .map(|e| call_at + e)
            .ok_or("air.intersect call without arguments")?;
        let tags = line[call_at + "@air.intersect.".len()..name_end].to_string();
        let family = crate::meta::AirIntersectionFamily::parse(&format!("air.intersect.{tags}"))?
            .ok_or("air.intersect family did not parse")?;
        let is_curve = family.curve_data;
        let res_name = line.split_once(" = ").map(|(r, _)| r.trim().to_string()).unwrap_or_default();
        let ret_ty_text = line[line.find("call ").map(|c| c + 5).unwrap_or(0)..call_at].trim().to_string();
        if curves && !is_curve && !reads_header_fields(&lines[i + 1..], &res_name, &ret_ty_text, &family) {
            i += 1;
            continue;
        }
        if family.primitive_motion || family.instance_motion {
            return Err(format!(
                "intersection tables: air.intersect.{tags} (motion) is not carried yet"
            ));
        }
        let close = line.rfind(')').ok_or("air.intersect call unterminated")?;
        let args = split_top(&line[name_end + 1..close]);
        if args.len() != family.argument_count() {
            return Err(format!(
                "ray tracing: air.intersect.{tags} has {} operands, expected {}",
                args.len(),
                family.argument_count()
            ));
        }
        let t = family.intersection_table_argument_index();
        let table_value = args[t].split_whitespace().last().unwrap_or("").to_string();
        let body_text = lines.join("\n");
        let is_null = table_value == "null"
            || defining_line(&body_text, &table_value)
                .is_some_and(|d| d.contains("@air.get_null_intersection_function_table"));
        if is_null != curves {
            i += 1;
            continue;
        }
        if args[t + 1].split_whitespace().last() != Some("null") {
            return Err("intersection tables: a ray payload is not carried yet".to_string());
        }
        let fns = if is_null { Vec::new() } else { intersection_functions(candidates, ids, &tags)? };
        // rtopt5: the table's kernel parameter by its position in the define (the metadata's `!{i32 <index>, ...}`):
        // front ends that keep value names (`%t.coerce`) gave no number here, the table kept its
        // intersection_function_table metadata, the emitter bound nothing and every slot read 0 (no function ran)
        if !is_null {
            match param_position(&lines[0], &table_value) {
                Some(n) => {
                    table_params.insert(n);
                }
                None => {
                    return Err(format!(
                        "intersection tables: the table operand {table_value} is not a parameter of {entry}"
                    ))
                }
            }
        }
        let (res, _) = line.split_once(" = ").ok_or("air.intersect result unnamed")?;
        let res = res.trim().to_string();
        let ret_ty = line[line.find("call ").unwrap() + 5..call_at].trim().to_string();
        let prefix = format!("{}{n}", if curves { "rtc" } else { "ift" });
        n += 1;
        let table = (!is_null).then(|| arg_value(&args[t]));
        let inline = emit_query_loop(&prefix, &res, &ret_ty, &tags, &args, &family, table.as_deref(), &fns, &mut declares)?;
        i += splice_inline(&mut lines, i, &prefix, &inline)?;
        if let Some(k) = family.result_fields().iter().position(|f| *f == crate::meta::AirIntersectionResultField::OpaquePointer) {
            let head = format!("extractvalue {ret_ty} {res}, {k}");
            for l in lines.iter_mut().skip(i) {
                if let Some((lhs, rhs)) = l.clone().split_once(" = ") {
                    if rhs.trim() == head || rhs.trim().starts_with(&format!("{head},")) {
                        *l = format!("{lhs} = inttoptr i64 %{prefix}.f{k}.a to ptr addrspace(1)");
                    }
                }
            }
        }
    }
    if n == 0 {
        return Ok(ll.to_string());
    }
    let mut out = String::with_capacity(ll.len() + 16384);
    out.push_str(&ll[..def_at]);
    out.push_str(&lines.join("\n"));
    out.push('\n');
    out.push_str(&ll[body_end..]);
    out.push('\n');
    let mut have: HashSet<String> = out
        .lines()
        .filter(|l| l.trim_start().starts_with("declare ") || l.trim_start().starts_with("define "))
        .filter_map(|l| l.find('@').map(|a| l[a..].split('(').next().unwrap_or("").to_string()))
        .collect();
    for d in declares {
        let g = d
            .find('@')
            .map(|a| d[a..].split('(').next().unwrap_or("").to_string())
            .unwrap_or_default();
        if have.insert(g) {
            out.push_str(&d);
            out.push('\n');
        }
    }
    for (_, module) in candidates {
        append_module_dedupe(&mut out, module);
    }
    append_rt_runtime(&mut out);
    let mut result = String::with_capacity(out.len() + 256);
    for line in out.lines() {
        let mut l = line.to_string();
        if l.trim_start().starts_with('!') && l.contains("!\"air.intersection_function_table\"") {
            let idx = l
                .split("!{i32 ")
                .nth(1)
                .and_then(|r| r.split(',').next())
                .and_then(|v| v.trim().parse::<u32>().ok());
            if idx.is_some_and(|i| table_params.contains(&i)) {
                l = l.replacen("!\"air.intersection_function_table\"", "!\"air.buffer\"", 1);
                let at = l
                    .find("!\"air.arg_type_name\", !\"intersection_function_table")
                    .ok_or_else(|| {
                        format!("intersection table metadata has an unexpected shape: {line}")
                    })?;
                let tail_start = at + "!\"air.arg_type_name\", !\"".len();
                let close = l[tail_start..]
                    .find('"')
                    .ok_or("intersection table metadata type name unterminated")?
                    + tail_start
                    + 1;
                l.replace_range(at..close, "!\"air.address_space\", i32 1, !\"air.arg_type_size\", i32 8, !\"air.arg_type_align_size\", i32 8, !\"air.arg_type_name\", !\"ulong\"");
            }
        }
        result.push_str(&l);
        result.push('\n');
    }
    Ok(result)
}

/// Whether the intersect result `res` (of type `ret_ty`) has its user instance ID or primitive data extracted.
fn reads_header_fields(lines: &[String], res: &str, ret_ty: &str, family: &crate::meta::AirIntersectionFamily) -> bool {
    use crate::meta::AirIntersectionResultField as Field;
    if res.is_empty() {
        return false;
    }
    let wanted: Vec<usize> = family
        .result_fields()
        .iter()
        .enumerate()
        .filter(|(_, f)| matches!(f, Field::UserInstanceId | Field::OpaquePointer))
        .map(|(k, _)| k)
        .collect();
    let head = format!("extractvalue {ret_ty} {res},");
    lines.iter().any(|l| {
        l.find(&head).is_some_and(|at| {
            l[at + head.len()..].trim().split(|c: char| !c.is_ascii_digit()).next().and_then(|k| k.parse::<usize>().ok()).is_some_and(|k| wanted.contains(&k))
        })
    })
}

/// Appends the ray tracing runtime (rt_runtime/runtime_ll.rs) once: types first, then its declarations and functions.
pub(crate) fn append_rt_runtime(out: &mut String) {
    if out.contains("@nvmtl_rt_curve(i64") && out.contains("define internal <4 x float> @nvmtl_rt_curve(") {
        return;
    }
    let rt = crate::rt_runtime::runtime_ll::RT_RUNTIME_LL;
    let (types, rest): (Vec<&str>, Vec<&str>) = rt.lines().partition(|l| l.starts_with("%struct.") && l.contains(" = type "));
    // named types go before the first function or declaration of the module
    let at = out
        .find("\ndefine ")
        .into_iter()
        .chain(out.find("\ndeclare "))
        .min()
        .map(|a| a + 1)
        .unwrap_or(out.len());
    let mut ty = String::new();
    for t in types {
        let name = t.split(" = ").next().unwrap_or("");
        if !out.contains(&format!("\n{name} = type ")) {
            ty.push_str(t);
            ty.push('\n');
        }
    }
    out.insert_str(at, &ty);
    append_module_dedupe(out, &rest.join("\n"));
}

fn arg_value(a: &str) -> String {
    let ty = arg_type(a).unwrap_or_default();
    a.trim()[ty.len().min(a.trim().len())..]
        .split_whitespace()
        .filter(|w| !matches!(*w, "readonly" | "noundef" | "nocapture" | "captures(none)" | "nonnull"))
        .collect::<Vec<_>>()
        .join(" ")
}

fn append_module_dedupe(output: &mut String, module: &str) {
    let already: HashSet<String> = output
        .lines()
        .filter_map(|l| {
            let t = l.trim_start();
            if t.starts_with("define ") || t.starts_with("declare ") {
                t.find('@')
                    .map(|at| t[at..].split('(').next().unwrap_or("").to_string())
            } else if t.starts_with('@') {
                t.split(" = ").next().map(|g| g.trim().to_string())
            } else {
                None
            }
        })
        .collect();
    let mut skipping = false;
    for line in module.lines() {
        let t = line.trim_start();
        if skipping {
            if t == "}" {
                skipping = false;
            }
            continue;
        }
        if t.starts_with("define ") || t.starts_with("declare ") {
            let g = t
                .find('@')
                .map(|at| t[at..].split('(').next().unwrap_or("").to_string())
                .unwrap_or_default();
            if already.contains(&g) {
                skipping = t.starts_with("define ") && !t.ends_with('}');
                continue;
            }
        } else if t.starts_with('@') {
            if let Some(g) = t.split(" = ").next() {
                if already.contains(g.trim()) {
                    continue;
                }
            }
        }
        if t.starts_with("; ModuleID =")
            || t.starts_with("source_filename =")
            || t.starts_with("target datalayout =")
            || t.starts_with("target triple =")
            || t.starts_with("attributes #")
            || t.starts_with('!')
        {
            continue;
        }
        output.push_str(line);
        output.push('\n');
    }
}

/// The query tags of an intersect's tags: curve_data dropped (rt_runtime carries curves on bounding boxes).
pub(crate) fn query_tags(tags: &str) -> String {
    let kept: Vec<&str> = tags.split('.').filter(|t| !t.is_empty() && *t != "curve_data").collect();
    if kept.is_empty() { String::new() } else { format!(".{}", kept.join(".")) }
}

/// An `air.intersect` call as an intersection query loop (LLVM text, lowered by ray_query.rs afterwards):
/// non-opaque triangles go to the table's triangle function (or are committed), bounding boxes are curve segments when
/// the family has curve_data and the structure's geometry is a curve (nvmtl_rt_curve: committed with the curve
/// parameter kept), else user boxes for the table's bounding-box functions (or skipped without a table). The table slot
/// is the instance's + the geometry's intersection function table offset (the structure's header, nvmtl_rt_ift_slot).
/// The result is built field by field from the committed hit; user IDs and primitive data come from the header.
#[allow(clippy::too_many_arguments)]
fn emit_query_loop(
    p: &str,
    res: &str,
    ret_ty: &str,
    tags: &str,
    args: &[String],
    family: &crate::meta::AirIntersectionFamily,
    table: Option<&str>,
    fns: &[IsectFn],
    declares: &mut Vec<String>,
) -> Result<String, String> {
    use crate::meta::{AirIntersectionInstancing as Inst, AirIntersectionResultField as Field};
    if family.instancing == Inst::MultiLevel {
        return Err("ray tracing: multi-level instancing is not supported on this device".into());
    }
    let tq = query_tags(tags);
    let curve = family.curve_data;
    let instancing = family.instancing == Inst::SingleLevel;
    let inst_c = u32::from(instancing);
    let q = |f: &str| format!("@air.{f}_intersection_query{tq}");
    let get = |what: &str, ty: &str, committed: bool, decl: &mut Vec<String>| -> String {
        let g = format!(
            "@air.get_{}_{what}_intersection_query{tq}",
            if committed { "committed" } else { "candidate" }
        );
        decl.push(format!("declare {ty} {g}(ptr)"));
        format!("call {ty} {g}(ptr %{p}.q)")
    };
    let mut names = Vec::new();
    for a in args.iter() {
        let ty = arg_type(a)?;
        names.push((ty, arg_value(a)));
    }
    let t = family.intersection_table_argument_index();
    let reset_idx: Vec<usize> = (0..t).chain(t + 3..args.len()).collect();
    // the operands as written (a structure loaded from an argument buffer keeps its readonly: the native emitter
    // resolves such a pointer only as a read-only call operand)
    let reset_args: Vec<String> = reset_idx.iter().map(|&i| args[i].trim().to_string()).collect();
    let reset_tys: Vec<String> = reset_idx
        .iter()
        .map(|&i| if args[i].contains(" readonly ") { format!("{} readonly", names[i].0) } else { names[i].0.clone() })
        .collect();
    let (tmin, tmax) = (names[2].1.clone(), names[3].1.clone());
    declares.push(format!("declare ptr {}()", q("allocate")));
    declares.push(format!("declare void {}(ptr, {})", q("reset"), reset_tys.join(", ")));
    declares.push(format!("declare i1 {}(ptr)", q("next")));
    declares.push(format!("declare i64 {}(ptr)", q("get_nvmtl_as_base")));
    declares.push(format!("declare void {}(ptr)", q("commit_triangle_intersection")));
    declares.push(format!("declare void {}(ptr, float)", q("commit_bounding_box_intersection")));
    let x = |n: &str| format!("%{p}.{n}");
    let mut s = String::new();
    s.push_str(&format!("{p}.entry:\n  %{p}.q = call ptr {}()\n", q("allocate")));
    s.push_str(&format!("  call void {}(ptr %{p}.q, {})\n", q("reset"), reset_args.join(", ")));
    s.push_str(&format!("  %{p}.as = call i64 {}(ptr %{p}.q)\n  br label %{p}.loop\n", q("get_nvmtl_as_base")));
    s.push_str(&format!(
        "{p}.loop:\n  %{p}.mu = phi float [ 0.000000e+00, %{p}.entry ], [ %{p}.mu.n, %{p}.latch ]\n  %{p}.mc = phi i32 [ 0, %{p}.entry ], [ %{p}.mc.n, %{p}.latch ]\n"
    ));
    s.push_str(&format!("  %{p}.more = call i1 {}(ptr %{p}.q)\n  br i1 %{p}.more, label %{p}.cand, label %{p}.done\n", q("next")));
    s.push_str(&format!("{p}.cand:\n"));
    s.push_str(&format!("  {} = {}\n", x("ty"), get("intersection_type", "i32", false, declares)));
    s.push_str(&format!("  {} = {}\n", x("gid"), get("geometry_id", "i32", false, declares)));
    if instancing {
        s.push_str(&format!("  {} = {}\n", x("iid"), get("instance_id", "i32", false, declares)));
    } else {
        s.push_str(&format!("  {} = add i32 0, 0\n", x("iid")));
    }
    if let Some(table) = table {
        s.push_str(&format!(
            "  {sl} = call i32 @nvmtl_rt_ift_slot(i64 %{p}.as, i32 {inst_c}, i32 {iid}, i32 {gid})\n",
            sl = x("slot"), iid = x("iid"), gid = x("gid")
        ));
        s.push_str(&format!("  {} = add i32 {}, {TABLE_BUFFERS}\n  {} = zext i32 {} to i64\n", x("slotw"), x("slot"), x("slot64"), x("slotw")));
        s.push_str(&format!(
            "  {} = getelementptr i64, ptr addrspace(1) {table}, i64 {}\n  {} = load i64, ptr addrspace(1) {}, align 8\n",
            x("idp"), x("slot64"), x("fid"), x("idp")
        ));
    }
    s.push_str(&format!("  {} = icmp eq i32 {}, 2\n  br i1 {}, label %{p}.box, label %{p}.tri\n", x("isbox"), x("ty"), x("isbox")));
    let boxes: Vec<&IsectFn> = fns.iter().filter(|f| f.bounding_box).collect();
    let tris: Vec<&IsectFn> = fns.iter().filter(|f| !f.bounding_box).collect();
    // triangles
    if table.is_some() {
        s.push_str(&format!(
            "{p}.tri:\n  switch i64 {}, label %{p}.tri_commit [ i64 0, label %{p}.tri_commit i64 {OPAQUE_TRIANGLE_ID}, label %{p}.tri_commit",
            x("fid")
        ));
        for f in &tris {
            s.push_str(&format!(" i64 {}, label %{p}.tri{}", f.id, f.id));
        }
        s.push_str(" ]\n");
    } else {
        s.push_str(&format!("{p}.tri:\n  br label %{p}.tri_commit\n"));
    }
    s.push_str(&format!("{p}.tri_commit:\n  call void {}(ptr %{p}.q)\n  br label %{p}.latch\n", q("commit_triangle_intersection")));
    // boxes: curve segments first
    s.push_str(&format!("{p}.box:\n"));
    if curve {
        s.push_str(&format!("  {} = {}\n", x("pid"), get("primitive_id", "i32", false, declares)));
        s.push_str(&format!("  {} = {}\n", x("ro"), get("ray_origin", "<3 x float>", false, declares)));
        s.push_str(&format!("  {} = {}\n", x("rd"), get("ray_direction", "<3 x float>", false, declares)));
        s.push_str(&format!("  {} = {}\n", x("bty"), get("intersection_type", "i32", true, declares)));
        s.push_str(&format!("  {} = {}\n", x("bd"), get("distance", "float", true, declares)));
        s.push_str(&format!(
            "  %{p}.bnone = icmp eq i32 %{p}.bty, 0\n  %{p}.bmax = select i1 %{p}.bnone, float {tmax}, float %{p}.bd\n"
        ));
        s.push_str(&format!(
            "  %{p}.cr = call <4 x float> @nvmtl_rt_curve(i64 %{p}.as, i32 {inst_c}, i32 %{p}.iid, i32 %{p}.gid, i32 %{p}.pid, <3 x float> %{p}.ro, <3 x float> %{p}.rd, float {tmin}, float %{p}.bmax)\n"
        ));
        s.push_str(&format!(
            "  %{p}.cst = extractelement <4 x float> %{p}.cr, i64 2\n  %{p}.chit = fcmp oeq float %{p}.cst, 2.000000e+00\n  br i1 %{p}.chit, label %{p}.cyes, label %{p}.cno\n"
        ));
        s.push_str(&format!(
            "{p}.cno:\n  %{p}.cnot = fcmp oeq float %{p}.cst, 0.000000e+00\n  br i1 %{p}.cnot, label %{p}.ubox, label %{p}.keep\n"
        ));
        s.push_str(&format!(
            "{p}.cyes:\n  %{p}.ct = extractelement <4 x float> %{p}.cr, i64 0\n  %{p}.cu = extractelement <4 x float> %{p}.cr, i64 1\n  call void {}(ptr %{p}.q, float %{p}.ct)\n  br label %{p}.latch\n",
            q("commit_bounding_box_intersection")
        ));
    } else {
        s.push_str(&format!("  br label %{p}.ubox\n"));
    }
    if table.is_some() {
        s.push_str(&format!("{p}.ubox:\n  switch i64 {}, label %{p}.keep [", x("fid")));
        for f in &boxes {
            s.push_str(&format!(" i64 {}, label %{p}.box{}", f.id, f.id));
        }
        s.push_str(" ]\n");
    } else {
        s.push_str(&format!("{p}.ubox:\n  br label %{p}.keep\n"));
    }
    s.push_str(&format!("{p}.keep:\n  br label %{p}.latch\n"));
    if !boxes.is_empty() {
        s.push_str(&format!("{p}.cleared:\n  br label %{p}.latch\n"));
    }
    for f in boxes.iter().chain(tris.iter()) {
        let lbl = format!("{p}.{}{}", if f.bounding_box { "box" } else { "tri" }, f.id);
        s.push_str(&format!("{lbl}:\n"));
        s.push_str(&format!("  %{lbl}.cty = {}\n", get("intersection_type", "i32", true, declares)));
        s.push_str(&format!("  %{lbl}.cd = {}\n", get("distance", "float", true, declares)));
        s.push_str(&format!(
            "  %{lbl}.none = icmp eq i32 %{lbl}.cty, 0\n  %{lbl}.max = select i1 %{lbl}.none, float {tmax}, float %{lbl}.cd\n"
        ));
        let mut call_args = Vec::new();
        for (k, (ty, role)) in f.params.iter().enumerate() {
            let v = format!("%{lbl}.p{k}");
            let line = match role {
                Role::Origin => format!("  {v} = {}\n", get("ray_origin", ty, false, declares)),
                Role::Direction => format!("  {v} = {}\n", get("ray_direction", ty, false, declares)),
                Role::MinDistance => format!("  {v} = fadd float {tmin}, 0.0\n"),
                Role::MaxDistance => format!("  {v} = fadd float %{lbl}.max, 0.0\n"),
                Role::PrimitiveId => format!("  {v} = {}\n", get("primitive_id", ty, false, declares)),
                Role::GeometryId => format!("  {v} = {}\n", get("geometry_id", ty, false, declares)),
                Role::InstanceId => format!("  {v} = {}\n", get("instance_id", ty, false, declares)),
                Role::UserInstanceId => format!("  {v} = {}\n", get("user_instance_id", ty, false, declares)),
                Role::WorldOrigin | Role::WorldDirection => {
                    let g = if *role == Role::WorldOrigin { "world_space_ray_origin" } else { "world_space_ray_direction" };
                    declares.push(format!("declare {ty} @air.get_{g}_intersection_query{tq}(ptr)"));
                    format!("  {v} = call {ty} @air.get_{g}_intersection_query{tq}(ptr %{p}.q)\n")
                }
                Role::Barycentric => format!("  {v} = {}\n", get("triangle_barycentric_coord", ty, false, declares)),
                Role::FrontFacing => format!("  {v} = {}\n", get("triangle_front_facing", ty, false, declares)),
                Role::Distance => format!("  {v} = {}\n", get("triangle_distance", ty, false, declares)),
                Role::PrimitiveData => format!(
                    "  {v}.p = {}\n  {v}.a = call i64 @nvmtl_rt_primitive_data(i64 %{p}.as, i32 {inst_c}, i32 %{p}.iid, i32 %{p}.gid, i32 {v}.p)\n  {v} = inttoptr i64 {v}.a to {ty}\n",
                    get("primitive_id", "i32", false, declares)
                ),
                Role::Buffer(loc) => format!(
                    "  {v}.at = getelementptr ptr addrspace(1), ptr addrspace(1) {}, i64 {loc}\n  {v} = load {ty}, ptr addrspace(1) {v}.at, align 8\n",
                    table.unwrap_or("null")
                ),
            };
            s.push_str(&line);
            call_args.push(format!("{ty} {v}"));
        }
        if f.bounding_box {
            s.push_str(&format!("  %{lbl}.r = call {} @{}({})\n", f.result_ty, f.symbol, call_args.join(", ")));
            s.push_str(&format!(
                "  %{lbl}.acc = extractvalue {} %{lbl}.r, 0\n  %{lbl}.t = extractvalue {} %{lbl}.r, 1\n",
                f.result_ty, f.result_ty
            ));
            s.push_str(&format!(
                "  %{lbl}.ge = fcmp oge float %{lbl}.t, {tmin}\n  %{lbl}.le = fcmp ole float %{lbl}.t, %{lbl}.max\n  %{lbl}.in = and i1 %{lbl}.ge, %{lbl}.le\n  %{lbl}.ok = and i1 %{lbl}.acc, %{lbl}.in\n"
            ));
            s.push_str(&format!("  br i1 %{lbl}.ok, label %{lbl}.commit, label %{p}.keep\n{lbl}.commit:\n"));
            s.push_str(&format!(
                "  call void {}(ptr %{p}.q, float %{lbl}.t)\n  br label %{p}.cleared\n",
                q("commit_bounding_box_intersection")
            ));
        } else {
            s.push_str(&format!("  %{lbl}.acc = call i1 @{}({})\n", f.symbol, call_args.join(", ")));
            s.push_str(&format!("  br i1 %{lbl}.acc, label %{p}.tri_commit, label %{p}.keep\n"));
        }
    }
    // the loop latch: the committed curve parameter and whether the committed hit is a curve
    let mut mu = vec![format!("[ %{p}.mu, %{p}.keep ]"), format!("[ %{p}.mu, %{p}.tri_commit ]")];
    let mut mc = vec![format!("[ %{p}.mc, %{p}.keep ]"), format!("[ 0, %{p}.tri_commit ]")];
    if !boxes.is_empty() {
        mu.push(format!("[ %{p}.mu, %{p}.cleared ]"));
        mc.push(format!("[ 0, %{p}.cleared ]"));
    }
    if curve {
        mu.push(format!("[ %{p}.cu, %{p}.cyes ]"));
        mc.push(format!("[ 1, %{p}.cyes ]"));
    }
    s.push_str(&format!(
        "{p}.latch:\n  %{p}.mu.n = phi float {}\n  %{p}.mc.n = phi i32 {}\n  br label %{p}.loop\n",
        mu.join(", "),
        mc.join(", ")
    ));
    // the result
    s.push_str(&format!("{p}.done:\n"));
    let members = split_top(ret_ty.trim().trim_start_matches('{').trim_end_matches('}'));
    let fields = family.result_fields();
    if members.len() != fields.len() {
        return Err(format!("ray tracing: result {ret_ty} does not match the {} fields of its family", fields.len()));
    }
    s.push_str(&format!("  %{p}.cty = {}\n", get("intersection_type", "i32", true, declares)));
    if curve {
        s.push_str(&format!(
            "  %{p}.cisb = icmp eq i32 %{p}.cty, 2\n  %{p}.cisc = icmp ne i32 %{p}.mc, 0\n  %{p}.cboth = and i1 %{p}.cisb, %{p}.cisc\n  %{p}.rty = select i1 %{p}.cboth, i32 3, i32 %{p}.cty\n"
        ));
    } else {
        s.push_str(&format!("  %{p}.rty = add i32 %{p}.cty, 0\n"));
    }
    let needs_ids = fields.iter().any(|f| matches!(f, Field::OpaquePointer | Field::UserInstanceId | Field::PrimitiveId | Field::GeometryId | Field::InstanceId));
    if needs_ids {
        s.push_str(&format!("  %{p}.rprim = {}\n", get("primitive_id", "i32", true, declares)));
        s.push_str(&format!("  %{p}.rgeo = {}\n", get("geometry_id", "i32", true, declares)));
        if instancing {
            s.push_str(&format!("  %{p}.rinst = {}\n", get("instance_id", "i32", true, declares)));
        } else {
            s.push_str(&format!("  %{p}.rinst = add i32 0, 0\n"));
        }
    }
    let mut agg = "undef".to_string();
    let mut transforms: [Option<String>; 2] = [None, None];
    for (k, field) in fields.iter().enumerate() {
        let mty = &members[k];
        let v = format!("%{p}.f{k}");
        match field {
            Field::IntersectionType => s.push_str(&format!("  {v} = add i32 %{p}.rty, 0\n")),
            Field::Distance => s.push_str(&format!("  {v} = {}\n", get("distance", mty, true, declares))),
            Field::PrimitiveId => s.push_str(&format!("  {v} = add i32 %{p}.rprim, 0\n")),
            Field::GeometryId => s.push_str(&format!("  {v} = add i32 %{p}.rgeo, 0\n")),
            Field::InstanceId => s.push_str(&format!("  {v} = add i32 %{p}.rinst, 0\n")),
            // the address only (a pointer does not travel through the aggregate in the native emitter): the result
            // holds null and its extractvalue reads become inttoptr of `{v}.a` (rewrite_intersects)
            Field::OpaquePointer => s.push_str(&format!(
                "  {v}.a = call i64 @nvmtl_rt_primitive_data(i64 %{p}.as, i32 {inst_c}, i32 %{p}.rinst, i32 %{p}.rgeo, i32 %{p}.rprim)\n"
            )),
            Field::UserInstanceId => s.push_str(&format!("  {v} = call i32 @nvmtl_rt_user_id(i64 %{p}.as, i32 %{p}.rinst)\n")),
            Field::InstanceLevel => return Err("ray tracing: multi-level instancing is not supported on this device".into()),
            Field::Barycentrics => s.push_str(&format!("  {v} = {}\n", get("triangle_barycentric_coord", mty, true, declares))),
            Field::FrontFacing => s.push_str(&format!("  {v} = {}\n", get("triangle_front_facing", mty, true, declares))),
            Field::CurveParameter => s.push_str(&format!("  {v} = select i1 %{p}.cboth, float %{p}.mu, float 0.000000e+00\n")),
            Field::WorldSpaceVector(c) => {
                let m = usize::from(*c / 4);
                let what = if m == 0 { "world_to_object_transform" } else { "object_to_world_transform" };
                let sty = format!("{{ {mty}, {mty}, {mty}, {mty} }}");
                if transforms[m].is_none() {
                    let tv = format!("%{p}.xf{m}");
                    s.push_str(&format!("  {tv} = {}\n", get(what, &sty, true, declares)));
                    transforms[m] = Some(tv);
                }
                s.push_str(&format!("  {v} = extractvalue {sty} {}, {}\n", transforms[m].as_ref().unwrap(), c % 4));
            }
        }
        let next = if k + 1 == fields.len() { res.to_string() } else { format!("%{p}.agg{k}") };
        let val = if *field == Field::OpaquePointer { "null".to_string() } else { v.clone() };
        s.push_str(&format!("  {next} = insertvalue {ret_ty} {agg}, {mty} {val}, {k}\n"));
        agg = next;
    }
    s.push_str(&format!("  br label %{p}.cont\n"));
    Ok(s)
}

#[cfg(test)]
mod tests {
    use super::*;

    const KERNEL: &str = r#"define void @k(ptr addrspace(1) %0, ptr addrspace(1) %1, ptr addrspace(1) %2) {
  %4 = tail call { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.instancing.triangle_data(<3 x float> zeroinitializer, <3 x float> <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float 0.000000e+00, float 1.000000e+01, ptr addrspace(1) readonly %0, i32 255, ptr addrspace(1) readonly %2, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 3, i32 -1, i32 -1, i32 0, i1 false, i1 false) #1
  %5 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %4, 1
  store float %5, ptr addrspace(1) %1, align 4
  ret void
}
"#;
    const SPHERE: &str = r#"define <{ i1, float }> @sphereIntersection(<3 x float> noundef %0, <3 x float> noundef %1, float noundef %2, float noundef %3, i32 noundef %4, ptr addrspace(1) noundef readonly "air-buffer-no-alias" %5) local_unnamed_addr #3 {
  %7 = insertvalue <{ i1, float }> undef, i1 true, 0
  %8 = insertvalue <{ i1, float }> %7, float %2, 1
  ret <{ i1, float }> %8
}
!air.intersection = !{!35}
!35 = !{ptr @sphereIntersection, !36, !39, !"air.bounding_box", !"air.instancing", !"air.triangle_data"}
!36 = !{!37, !38}
!37 = !{!"air.accept_intersection", !"air.arg_type_name", !"bool", !"air.arg_name", !"accept"}
!38 = !{!"air.distance", !"air.arg_type_name", !"float", !"air.arg_name", !"distance"}
!39 = !{!40, !41, !42, !43, !44, !45}
!40 = !{i32 0, !"air.origin", !"air.arg_type_name", !"float3", !"air.arg_name", !"origin"}
!41 = !{i32 1, !"air.direction", !"air.arg_type_name", !"float3", !"air.arg_name", !"direction"}
!42 = !{i32 2, !"air.min_distance", !"air.arg_type_name", !"float", !"air.arg_name", !"minDistance"}
!43 = !{i32 3, !"air.max_distance", !"air.arg_type_name", !"float", !"air.arg_name", !"maxDistance"}
!44 = !{i32 4, !"air.primitive_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"prim"}
!45 = !{i32 5, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"spheres"}
"#;
    const ALPHA: &str = r#"define i1 @alphaTest(i32 noundef %0, <2 x float> noundef %1) local_unnamed_addr #4 {
  %3 = and i32 %0, 1
  %4 = icmp eq i32 %3, 0
  ret i1 %4
}
!air.intersection = !{!46}
!46 = !{ptr @alphaTest, !47, !49, !"air.triangle", !"air.instancing", !"air.triangle_data"}
!47 = !{!48}
!48 = !{!"air.accept_intersection", !"air.arg_type_name", !"bool"}
!49 = !{!50, !51}
!50 = !{i32 0, !"air.primitive_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"prim"}
!51 = !{i32 1, !"air.barycentric_coord", !"air.arg_type_name", !"float2", !"air.arg_name", !"bary", !"air.arg_unused"}
"#;

    #[test]
    fn a_table_bearing_intersect_becomes_a_query_loop_calling_both_linked_functions() {
        let cands = vec![
            ("sphereIntersection".to_string(), SPHERE.to_string()),
            ("alphaTest".to_string(), ALPHA.to_string()),
        ];
        let out = rewrite_runtime_intersection_tables(KERNEL, "k", &cands).unwrap();
        assert!(!out.contains("call { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect."), "{out}");
        assert!(out.contains("%ift0.q = call ptr @air.allocate_intersection_query"), "{out}");
        assert!(out.contains("ift0.cont:"), "{out}");
        assert!(out.contains("i64 1, label %ift0.box1"), "{out}");
        assert!(out.contains("i64 2, label %ift0.tri2"), "{out}");
        assert!(
            out.contains("call <{ i1, float }> @sphereIntersection("),
            "{out}"
        );
        assert!(out.contains("call i1 @alphaTest("), "{out}");
        assert!(
            out.contains("call i32 @nvmtl_rt_ift_slot(i64 %ift0.as, i32 1, i32 %ift0.iid, i32 %ift0.gid)"),
            "the slot is the instance's + the geometry's table offset from the header: {out}"
        );
        assert!(out.contains("define internal <4 x float> @nvmtl_rt_curve("), "the runtime is appended: {out}");
        assert!(
            out.contains("define <{ i1, float }> @sphereIntersection"),
            "the candidate body is appended: {out}"
        );
    }

    #[test]
    fn the_table_argument_becomes_a_u64_buffer_at_its_location() {
        let k = format!("{KERNEL}!0 = !{{i32 2, !\"air.intersection_function_table\", !\"air.location_index\", i32 3, i32 1, !\"air.read_write\", !\"air.arg_type_name\", !\"intersection_function_table<instancing, triangle_data>\", !\"air.arg_name\", !\"table\"}}\n");
        let cands = vec![("sphereIntersection".to_string(), SPHERE.to_string())];
        let out = rewrite_runtime_intersection_tables(&k, "k", &cands).unwrap();
        assert!(out.contains("!0 = !{i32 2, !\"air.buffer\", !\"air.location_index\", i32 3, i32 1, !\"air.read_write\", !\"air.address_space\", i32 1, !\"air.arg_type_size\", i32 8, !\"air.arg_type_align_size\", i32 8, !\"air.arg_type_name\", !\"ulong\", !\"air.arg_name\", !\"table\"}"), "{out}");
    }

    /// rtopt5: a front end that keeps value names (`%t.coerce`): the table is still found by its parameter position
    /// (metal-rt-curves ift on the B580: no binding for the table, every slot read 0, distance -1 for both rays)
    #[test]
    fn a_named_table_parameter_becomes_a_u64_buffer_at_its_location() {
        let k = KERNEL
            .replace("define void @k(ptr addrspace(1) %0, ptr addrspace(1) %1, ptr addrspace(1) %2) {",
                     "define void @k(ptr addrspace(1) %as.coerce, ptr addrspace(1) %out, ptr addrspace(1) %t.coerce) {\nentry:")
            .replace("readonly %0, i32 255, ptr addrspace(1) readonly %2", "readonly %as.coerce, i32 255, ptr addrspace(1) readonly %t.coerce")
            .replace("ptr addrspace(1) %1, align 4", "ptr addrspace(1) %out, align 4");
        let k = format!("{k}!0 = !{{i32 2, !\"air.intersection_function_table\", !\"air.location_index\", i32 3, i32 1, !\"air.read_write\", !\"air.arg_type_name\", !\"intersection_function_table<instancing, triangle_data>\", !\"air.arg_name\", !\"t\"}}\n");
        let cands = vec![("sphereIntersection".to_string(), SPHERE.to_string())];
        let out = rewrite_runtime_intersection_tables(&k, "k", &cands).unwrap();
        assert!(out.contains("!0 = !{i32 2, !\"air.buffer\", !\"air.location_index\", i32 3, i32 1, !\"air.read_write\", !\"air.address_space\", i32 1"), "{out}");
        assert!(out.contains("getelementptr i64, ptr addrspace(1) %t.coerce, i64 %ift0.slot64"), "{out}");
        assert!(!out.contains("air.intersection_function_table"), "{out}");
    }

    #[test]
    fn parameter_positions_and_the_entry_block_number() {
        let d = "define void @k(ptr addrspace(1) noundef readonly %as.coerce, ptr addrspace(1) %1, i32 noundef %i, <2 x i32> %3) local_unnamed_addr #0 {";
        assert_eq!(param_position(d, "%as.coerce"), Some(0));
        assert_eq!(param_position(d, "%1"), Some(1));
        assert_eq!(param_position(d, "%i"), Some(2));
        assert_eq!(param_position(d, "%t"), None);
        assert_eq!(unnamed_param_count(d), 2);
        assert_eq!(unnamed_param_count("define void @k(ptr %0, i32 %1) {"), 2);
    }

    #[test]
    fn a_null_table_intersect_is_left_alone() {
        let k = KERNEL.replace(
            "ptr addrspace(1) readonly %2, ptr null",
            "ptr addrspace(1) readonly null, ptr null",
        );
        let out = rewrite_runtime_intersection_tables(&k, "k", &[]).unwrap();
        assert_eq!(out, k);
    }

    #[test]
    fn primitive_data_and_user_ids_come_from_the_header() {
        let k = KERNEL.replace(
            "%5 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %4, 1",
            "%5 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %4, 4",
        );
        let cands = vec![("sphereIntersection".to_string(), SPHERE.to_string())];
        let out = rewrite_runtime_intersection_tables(&k, "k", &cands).unwrap();
        assert!(out.contains("call i64 @nvmtl_rt_primitive_data(i64 %ift0.as, i32 1, i32 %ift0.rinst, i32 %ift0.rgeo, i32 %ift0.rprim)"), "{out}");
        assert!(out.contains("call i32 @nvmtl_rt_user_id(i64 %ift0.as, i32 %ift0.rinst)"), "{out}");
    }

    #[test]
    fn a_curve_intersect_without_a_table_becomes_a_query_loop_with_the_curve_runtime() {
        let k = r#"define void @c(ptr addrspace(1) %as, ptr addrspace(1) %out) {
  %t = tail call ptr addrspace(1) @air.get_null_intersection_function_table()
  %r = tail call { i32, float, i32, i32, ptr addrspace(1), float } @air.intersect.curve_data(<3 x float> zeroinitializer, <3 x float> <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float 0.000000e+00, float 1.000000e+02, ptr addrspace(1) readonly %as, ptr addrspace(1) readonly %t, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 4, i32 -1, i32 -1, i32 0, i1 false)
  %u = extractvalue { i32, float, i32, i32, ptr addrspace(1), float } %r, 5
  store float %u, ptr addrspace(1) %out, align 4
  ret void
}
"#;
        let out = rewrite_curve_intersects(k, "c").unwrap();
        assert!(out.contains("call void @air.reset_intersection_query(ptr %rtc0.q, <3 x float> zeroinitializer"), "{out}");
        assert!(out.contains("call <4 x float> @nvmtl_rt_curve(i64 %rtc0.as, i32 0, i32 %rtc0.iid, i32 %rtc0.gid, i32 %rtc0.pid"), "{out}");
        assert!(out.contains("%rtc0.rty = select i1 %rtc0.cboth, i32 3, i32 %rtc0.cty"), "{out}");
        assert!(out.contains("%r = insertvalue { i32, float, i32, i32, ptr addrspace(1), float } %rtc0.agg4, float %rtc0.f5, 5"), "{out}");
        assert!(!out.contains("call { i32, float, i32, i32, ptr addrspace(1), float } @air.intersect."), "{out}");
    }

    #[test]
    fn a_payload_is_refused_by_name() {
        let k = KERNEL.replace("ptr null, i64 0", "ptr %1, i64 16");
        let err = rewrite_runtime_intersection_tables(&k, "k", &[]).unwrap_err();
        assert!(err.contains("payload"), "{err}");
    }
}
