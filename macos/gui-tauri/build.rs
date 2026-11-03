fn main() {
  println!("cargo:rerun-if-env-changed=MACOSX_DEPLOYMENT_TARGET");
  tauri_build::build()
}
