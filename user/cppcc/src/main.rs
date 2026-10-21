fn main() {
	match cppcc::driver::run(std::env::args().skip(1)) {
		Ok(code) => std::process::exit(code),
		Err(error) => {
			eprintln!("cppcc: {:#}", error);
			std::process::exit(2);
		}
	}
}
