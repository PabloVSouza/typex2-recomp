//! New launcher releases on GitHub: a release build looks at the latest stable release; a
//! release-candidate build at every recent release, candidates included. Nothing is downloaded:
//! the launcher shows the notes and opens the release page.
use reqwest::StatusCode;
use semver::Version;
use serde::{Deserialize, Serialize};
use std::{cmp::Ordering, time::Duration};
use tauri_plugin_opener::OpenerExt;

const LATEST: &str = "https://api.github.com/repos/PabloVSouza/typex2-recomp/releases/latest";
/// Recent releases, candidates included, newest first (drafts are not listed publicly).
const LIST: &str = "https://api.github.com/repos/PabloVSouza/typex2-recomp/releases?per_page=20";
const RELEASE_PAGE: &str = "https://github.com/PabloVSouza/typex2-recomp/releases/tag/";
const MAX_RESPONSE: usize = 512 * 1024;

#[derive(Deserialize)]
struct GithubRelease {
    tag_name: String,
    draft: bool,
    prerelease: bool,
    #[serde(default)]
    body: Option<String>,
}

#[derive(Debug, Serialize)]
pub struct ReleaseInfo {
    version: String,
    tag: String,
    newer: bool,
    notes: String,
}

/// The installed launcher's version: the tag a release build was made from (`X2_RELEASE_TAG`, set
/// by release.yml), which names a candidate (`v0.2.0-rc.1`) where the version files carry only the
/// release's number; otherwise the crate's.
pub fn installed_version() -> &'static str {
    option_env!("X2_RELEASE_TAG")
        .map(|tag| tag.strip_prefix('v').unwrap_or(tag))
        .filter(|v| Version::parse(v).is_ok())
        .unwrap_or(env!("CARGO_PKG_VERSION"))
}

fn is_candidate(installed: &str) -> bool {
    Version::parse(installed).is_ok_and(|v| !v.pre.is_empty())
}

fn stable_version(tag: &str) -> Result<Version, String> {
    let v = Version::parse(tag.strip_prefix('v').unwrap_or(tag)).map_err(|_| "The release tag is not a version.".to_string())?;
    if !v.pre.is_empty() {
        return Err("The release tag is a prerelease.".into());
    }
    Ok(v)
}

fn decode_latest(status: StatusCode, body: &[u8], current: &str) -> Result<Option<ReleaseInfo>, String> {
    if status == StatusCode::NOT_FOUND {
        return Ok(None); // no release yet
    }
    if !status.is_success() {
        return Err(format!("GitHub answered HTTP {status}."));
    }
    let release: GithubRelease = serde_json::from_slice(body).map_err(|_| "GitHub sent an invalid answer.".to_string())?;
    if release.draft || release.prerelease {
        return Ok(None);
    }
    let version = stable_version(&release.tag_name)?;
    let installed = Version::parse(current).map_err(|_| "The installed version is invalid.".to_string())?;
    Ok(Some(ReleaseInfo {
        newer: version.cmp_precedence(&installed) == Ordering::Greater,
        version: version.to_string(),
        tag: release.tag_name,
        notes: release.body.unwrap_or_default(),
    }))
}

/// A candidate's check: the highest release listed, candidates included.
fn decode_list(status: StatusCode, body: &[u8], current: &str) -> Result<Option<ReleaseInfo>, String> {
    if !status.is_success() {
        return Err(format!("GitHub answered HTTP {status}."));
    }
    let releases: Vec<GithubRelease> = serde_json::from_slice(body).map_err(|_| "GitHub sent an invalid answer.".to_string())?;
    let installed = Version::parse(current).map_err(|_| "The installed version is invalid.".to_string())?;
    let newest = releases
        .into_iter()
        .filter(|r| !r.draft)
        .filter_map(|r| Some((Version::parse(r.tag_name.strip_prefix('v')?).ok()?, r)))
        .max_by(|a, b| a.0.cmp_precedence(&b.0));
    Ok(newest.map(|(version, r)| ReleaseInfo {
        newer: version.cmp_precedence(&installed) == Ordering::Greater,
        version: version.to_string(),
        tag: r.tag_name,
        notes: r.body.unwrap_or_default(),
    }))
}

#[tauri::command]
pub async fn check_release() -> Result<Option<ReleaseInfo>, String> {
    let installed = installed_version();
    let candidate = is_candidate(installed);
    let client = reqwest::Client::builder()
        .https_only(true)
        .redirect(reqwest::redirect::Policy::none())
        .connect_timeout(Duration::from_secs(5))
        .timeout(Duration::from_secs(10))
        .user_agent(concat!("typex2-launcher/", env!("CARGO_PKG_VERSION")))
        .build()
        .map_err(|e| e.to_string())?;
    let mut response = client
        .get(if candidate { LIST } else { LATEST })
        .header("Accept", "application/vnd.github+json")
        .send()
        .await
        .map_err(|e| e.to_string())?;
    let status = response.status();
    let decode = if candidate { decode_list } else { decode_latest };
    if !status.is_success() {
        return decode(status, &[], installed);
    }
    let mut body = Vec::new();
    while let Some(chunk) = response.chunk().await.map_err(|e| e.to_string())? {
        if chunk.len() > MAX_RESPONSE - body.len() {
            return Err("GitHub's answer is too large.".into());
        }
        body.extend_from_slice(&chunk);
    }
    decode(status, &body, installed)
}

/// A release's page: the tag must be a whole version (a stable one, unless this build is a
/// candidate), so the link cannot leave the project's release pages.
fn release_url(tag: &str, candidate: bool) -> Result<String, String> {
    if candidate {
        Version::parse(tag.strip_prefix('v').ok_or("The release tag is not a version.")?)
            .map_err(|_| "The release tag is not a version.".to_string())?;
    } else {
        stable_version(tag)?;
    }
    Ok(format!("{RELEASE_PAGE}{tag}"))
}

#[tauri::command]
pub fn open_release(app: tauri::AppHandle, tag: String) -> Result<(), String> {
    let url = release_url(&tag, is_candidate(installed_version()))?;
    app.opener().open_url(url, None::<&str>).map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn release(tag: &str) -> Vec<u8> {
        serde_json::to_vec(&serde_json::json!({"tag_name": tag, "draft": false, "prerelease": false})).unwrap()
    }

    #[test]
    fn newer_by_version_precedence() {
        for (tag, current, newer) in [
            ("v0.1.1", "0.1.0", true),
            ("v0.10.0", "0.9.0", true),
            ("v0.1.0", "0.1.1", false),
            ("v0.1.0", "0.1.0", false),
            ("v0.1.0", "0.1.0-rc.3", true),
        ] {
            assert_eq!(decode_latest(StatusCode::OK, &release(tag), current).unwrap().unwrap().newer, newer, "{tag} vs {current}");
        }
    }

    #[test]
    fn drafts_prereleases_and_failures() {
        for flag in ["draft", "prerelease"] {
            let mut v: serde_json::Value = serde_json::from_slice(&release("v0.2.0")).unwrap();
            v[flag] = true.into();
            assert!(decode_latest(StatusCode::OK, &serde_json::to_vec(&v).unwrap(), "0.1.0").unwrap().is_none());
        }
        assert!(decode_latest(StatusCode::NOT_FOUND, &[], "0.1.0").unwrap().is_none());
        for status in [StatusCode::FORBIDDEN, StatusCode::TOO_MANY_REQUESTS, StatusCode::INTERNAL_SERVER_ERROR] {
            assert!(decode_latest(status, &[], "0.1.0").is_err());
        }
        for body in [b"{}".to_vec(), release("latest"), release("v0.2.0-beta")] {
            assert!(decode_latest(StatusCode::OK, &body, "0.1.0").is_err());
        }
    }

    #[test]
    fn candidates_see_newer_candidates_and_the_final_release() {
        let list = |rs: &[(&str, bool)]| {
            serde_json::to_vec(
                &rs.iter()
                    .map(|(tag, draft)| serde_json::json!({"tag_name": tag, "draft": draft, "prerelease": tag.contains('-')}))
                    .collect::<Vec<_>>(),
            )
            .unwrap()
        };
        let newest = |rs: &[(&str, bool)], cur: &str| decode_list(StatusCode::OK, &list(rs), cur).unwrap().map(|r| (r.tag, r.newer));
        assert_eq!(newest(&[("v0.2.0", false), ("v0.2.0-rc.2", false)], "0.2.0-rc.2"), Some(("v0.2.0".into(), true)));
        assert_eq!(newest(&[("v0.2.0-rc.3", false), ("v0.1.0", false)], "0.2.0-rc.2"), Some(("v0.2.0-rc.3".into(), true)));
        assert_eq!(newest(&[("v0.2.0-rc.2", false)], "0.2.0-rc.2"), Some(("v0.2.0-rc.2".into(), false)));
        assert_eq!(newest(&[("v1.0.0", true), ("latest", false)], "0.2.0-rc.2"), None);
        assert!(is_candidate("0.2.0-rc.2") && !is_candidate("0.2.0"));
    }

    #[test]
    fn links_stay_on_the_release_pages() {
        assert_eq!(release_url("v0.1.1", false).unwrap(), format!("{RELEASE_PAGE}v0.1.1"));
        for tag in ["https://example.com", "../../evil", "v0.1.1?x=1", "v0.1.1/evil", "v0.1.1-rc.1"] {
            assert!(release_url(tag, false).is_err(), "{tag}");
        }
        assert!(release_url("v0.1.1-rc.1", true).is_ok());
        for tag in ["https://example.com", "v0.1.1/evil", "0.1.1"] {
            assert!(release_url(tag, true).is_err(), "{tag}");
        }
    }
}
