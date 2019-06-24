# typed: __STDLIB_INTERNAL

module Profiler__
  sig { params(io: IO).void }
  def self.print_profile(io); end

  sig { void }
  def self.start_profile; end

  sig { void }
  def self.stop_profile; end
end
