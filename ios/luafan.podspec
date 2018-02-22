Pod::Spec.new do |s|
  s.name         = "luafan"
  s.version      = "0.6.7"
  s.summary      = "luafan library."
  s.description  = <<-DESC
                   luafan library.
                   DESC
  s.homepage     = "https://github.com/luafan/luafan"
  s.license      = "MIT"
  s.author       = { "samchang" => "sam.chang@me.com" }
  s.platform     = :ios, "7.0.0"
  s.source       = { :git => "https://github.com/luafan/luafan.git", :tag => "v#{s.version}" }
  
  s.source_files  = "src/*.{h,c}", "src/utlua.c"
  s.exclude_files = "src/luamariadb.c"
  s.compiler_flags = '-DTARGET_OS_IPHONE=1'

  s.dependency 'CAPKit-lua53', '~> 0.1.0'
  s.dependency 'CAPKit-3rdparty-libs', '~> 0.1.0'

  s.xcconfig = { "HEADER_SEARCH_PATHS" => "$(PODS_ROOT)/#{s.name}/src $(PODS_ROOT)/CAPKit-lua53/lua-5.3.3/src $(PODS_ROOT)/CAPKit-lua53/lua53" }

end