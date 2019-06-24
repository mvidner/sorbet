# typed: true
ENV.to_hash
ENV.to_hash.merge({})

ENV.replace("VAR" => "val")
ENV.replace("VAR" => "val").merge({}) # error: Method merge does not exist on Sorbet::Private::Static::ENVClass 
