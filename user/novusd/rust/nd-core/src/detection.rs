use crate::types::{Detection, ScanResult, Verdict};

pub fn combine(
    hits: &[ScanResult],
    heuristic_score: u32,
    fuzzy: Option<(u32, u32)>,
    mut base: Detection,
) -> Detection {
    let mut max_conf = 0u32;
    let mut best_rule = 0i32;
    let mut verdict = Verdict::Clean;
    for h in hits {
        let v = Verdict::from_i32(h.verdict);
        if h.confidence > max_conf {
            max_conf = h.confidence;
            best_rule = h.rule_id;
        }
        if v == Verdict::Malicious {
            verdict = Verdict::Malicious;
        } else if v == Verdict::Suspicious && verdict != Verdict::Malicious {
            verdict = Verdict::Suspicious;
        }
    }

    if verdict == Verdict::Clean {
        if let Some((id, pct)) = fuzzy {
            if pct >= 80 {
                verdict = Verdict::Suspicious;
                max_conf = max_conf.max(pct);
                best_rule = id as i32;
                base.fuzzy_match = true;
            }
        }
    }
    if verdict == Verdict::Clean && heuristic_score >= 70 {
        verdict = Verdict::Suspicious;
        max_conf = max_conf.max(heuristic_score);
    }

    base.verdict = verdict;
    base.confidence = max_conf;
    if best_rule != 0 {
        base.rule_id = best_rule;
    }
    base.heuristic_score = heuristic_score;
    base
}

pub fn is_malicious(v: Verdict) -> bool {
    v == Verdict::Malicious
}
