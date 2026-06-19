require 'json'

parser = JSON::ResumableParser.new
parser << '[' # unterminated array

puts RUBY_DESCRIPTION
if parser.parse
  puts "BUG: #parse returned true"
  exit 1
else
  puts "OK"
end
