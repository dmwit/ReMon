#!/usr/bin/env ruby

require 'pty'

@poresults = Hash.new
@toresults = Hash.new
@inputset  = "test"
@workers   = [4]
@variants  = [2]
@parsec    = (30..42)
@runs      = 1

def get_bench_name(benchnum)
  _benchname=`grep "REGISTER.*(#{benchnum}," ../../Src/MVEE_demos.cpp`.split('"')[3]
  return _benchname if _benchname
  "dunno"
end


def run_bench(benchnum, variants, threads, input, results, native)
  benchname=get_bench_name(benchnum)
  if native
      results[threads] = Hash.new if not results[threads]
      results[threads][benchname] = Hash.new if not results[threads][benchname]
      results[threads][benchname][0] = Array.new if not results[threads][benchname][0]
      results[threads][benchname][0] << 0.0
    return
  end
    
  PTY.spawn("./MVEE #{benchnum} #{variants} #{threads} #{input} #{'-n' if native} 2>&1") do |stdout, stdin, pid|
    begin
      stdout.each { |ln|
        if ln.match(/real\t/)
          time = ln.split("\t")[1].chop 
          seconds = Float(time.split("m")[0].to_i * 60) + Float(time.split("m")[1].chop)
          
          variants = 0 if native
          results[threads] = Hash.new if not results[threads]
          results[threads][benchname] = Hash.new if not results[threads][benchname]
          results[threads][benchname][variants] = Array.new if not results[threads][benchname][variants]
          results[threads][benchname][variants] << seconds
          
          print("        #{seconds.to_s.gsub(".", ",")}\n")
        else 
          if ln.match(/ERROR/)
            print("ERROR: #{ln}\n")
            `killall -9 MVEE MVEE_LD_Loader_`
          end
        end
      }
    rescue Errno::EIO
    end
  end
end

def print_spreadsheet(file, str)
  file.write(str)
  print(str)
end

def dump_spreadsheet(results, prefix, threads)
  return if not results[threads]

  print("Spreadsheet for #{threads} worker threads:\n\n")

  File.open("parsec_#{prefix}_#{threads}_workers.csv", "w") { |file|
    columns="Benchmark;Native;"
    @variants.each { |variants|
      columns << "GHUMVEE (#{variants} Variants);"
    }
    print_spreadsheet(file, columns + "\n")

    results[threads].each { |benchname, benchtable|
      print_spreadsheet(file, benchname + ";")
      benchtable.each_value { |arr|
        avg = 0.0
        arr.each { |val|
          avg += val
        }
        avg /= arr.size
        print_spreadsheet(file, "#{avg.to_s.gsub(".", ",")};")
      }
      print_spreadsheet(file, "\n")
    }

    print("\n")
  }
end

@workers.each { |threads|
  print("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n")
  print("@" + "PARSEC 2.1 - #{threads} WORKER THREADS".center(78) + "@\n")
  print("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n\n")

#  install_orig_libc()
  print("Native:\n")

  @parsec.each { |num| 
    print("    running benchmark: #{get_bench_name(num)}\n")
    (1..@runs).each { run_bench(num, 1, threads, @inputset, @poresults, true) }
  }

  print("\n")

#  install_partialorder_libc()
  @variants.each { |variants|
    print("#{variants} variants:\n")
    @parsec.each { |num| 
      print("    running benchmark: #{get_bench_name(num)}\n")
      (1..@runs).each { run_bench(num, variants, threads, @inputset, @poresults, false) }
    }

    print("\n")
  }

  # install_totalorder_libc()
  # # copy results of the native benchmark
  # @toresults[threads] = Hash.new
  # @poresults[threads].each { |benchname, benchtable|
  #   @toresults[threads][benchname] = Hash.new
  #   @toresults[threads][benchname][0] = benchtable[0]
  # }
  # @variants.each { |variants|
  #   print("#{variants} variants:\n")
  #   @parsec.each { |num| 
  #     print("    running benchmark: #{get_bench_name(num)}\n")
  #     (1..@runs).each { run_bench(num, variants, threads, @inputset, @toresults, false) }
  #   }

  #   print("\n")
  # }

  print("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n")
}

@workers.each { |threads|
  dump_spreadsheet(@poresults, "partialorder", threads)
#  dump_spreadsheet(@toresults, "totalorder", threads)
}
